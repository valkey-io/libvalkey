
[![Build Status](https://github.com/redis/hiredis/actions/workflows/build.yml/badge.svg)](https://github.com/redis/hiredis/actions/workflows/build.yml)

**This Readme reflects the latest changed in the master branch. See [v1.0.0](https://github.com/redis/hiredis/tree/v1.0.0) for the Readme and documentation for the latest release ([API/ABI history](https://abi-laboratory.pro/?view=timeline&l=hiredis)).**

# LIBVALKEY

Libvalkey is a minimalistic C client library for the [Valkey](https://valkey.io/) database.

It is minimalistic because it just adds minimal support for the protocol, but
at the same time it uses a high level printf-alike API in order to make it
much higher level than otherwise suggested by its minimal code base and the
lack of explicit bindings for every Valkey command.

Apart from supporting sending commands and receiving replies, it comes with
a reply parser that is decoupled from the I/O layer. It
is a stream parser designed for easy reusability, which can for instance be used
in higher level language bindings for efficient reply parsing.

Libvalkey only supports the binary-safe RESP protocol, so you can use it with any
server that uses it.  

The library comes with multiple APIs. There is the
*synchronous API*, the *asynchronous API* and the *reply parsing API*.

## Synchronous API

To consume the synchronous API, there are only a few function calls that need to be introduced:

```c
valkeyContext *valkeyConnect(const char *ip, int port);
void *valkeyCommand(valkeyContext *c, const char *format, ...);
void freeReplyObject(void *reply);
```

### Connecting

The function `valkeyConnect` is used to create a so-called `valkeyContext`. The
context is where Libvalkey holds state for a connection. The `valkeyContext`
struct has an integer `err` field that is non-zero when the connection is in
an error state. The field `errstr` will contain a string with a description of
the error. More information on errors can be found in the **Errors** section.
After trying to connect to Valkey using `valkeyConnect` you should
check the `err` field to see if establishing the connection was successful:

```c
valkeyContext *c = valkeyConnect("127.0.0.1", 6379);
if (c == NULL || c->err) {
    if (c) {
        printf("Error: %s\n", c->errstr);
        // handle error
    } else {
        printf("Can't allocate valkey context\n");
    }
}
```

One can also use `valkeyConnectWithOptions` which takes a `valkeyOptions` argument
that can be configured with endpoint information as well as many different flags
to change how the `valkeyContext` will be configured.

```c
valkeyOptions opt = {0};

/* One can set the endpoint with one of our helper macros */
if (tcp) {
    VALKEY_OPTIONS_SET_TCP(&opt, "localhost", 6379);
} else {
    VALKEY_OPTIONS_SET_UNIX(&opt, "/tmp/valkey.sock");
}

/* And privdata can be specified with another helper */
VALKEY_OPTIONS_SET_PRIVDATA(&opt, myPrivData, myPrivDataDtor);

/* Finally various options may be set via the `options` member, as described below */
opt->options |= VALKEY_OPT_PREFER_IPV4;
```

If a connection is lost, `int valkeyReconnect(valkeyContext *c)` can be used to restore the connection using the same endpoint and options as the given context.

### Configurable valkeyOptions flags

There are several flags you may set in the `valkeyOptions` struct to change default behavior.  You can specify the flags via the `valkeyOptions->options` member.

| Flag | Description  |
| --- | --- |
| VALKEY\_OPT\_NONBLOCK | Tells libvalkey to make a non-blocking connection. |
| VALKEY\_OPT\_REUSEADDR | Tells libvalkey to set the [SO_REUSEADDR](https://man7.org/linux/man-pages/man7/socket.7.html) socket option |
| VALKEY\_OPT\_PREFER\_IPV4<br>VALKEY\_OPT\_PREFER_IPV6<br>VALKEY\_OPT\_PREFER\_IP\_UNSPEC | Informs libvalkey to either prefer IPv4 or IPv6 when invoking [getaddrinfo](https://man7.org/linux/man-pages/man3/gai_strerror.3.html).  `VALKEY_OPT_PREFER_IP_UNSPEC` will cause libvalkey to specify `AF_UNSPEC` in the getaddrinfo call, which means both IPv4 and IPv6 addresses will be searched simultaneously.<br>Libvalkey prefers IPv4 by default. |
| VALKEY\_OPT\_NO\_PUSH\_AUTOFREE | Tells libvalkey to not install the default RESP3 PUSH handler (which just intercepts and frees the replies).  This is useful in situations where you want to process these messages in-band. |
| VALKEY\_OPT\_NOAUTOFREEREPLIES | **ASYNC**: tells libvalkey not to automatically invoke `freeReplyObject` after executing the reply callback. |
| VALKEY\_OPT\_NOAUTOFREE | **ASYNC**: Tells libvalkey not to automatically free the `valkeyAsyncContext` on connection/communication failure, but only if the user makes an explicit call to `valkeyAsyncDisconnect` or `valkeyAsyncFree` |

*Note: A `valkeyContext` is not thread-safe.*

### Other configuration using socket options

The following socket options are applied directly to the underlying socket.
The values are not stored in the `valkeyContext`, so they are not automatically applied when reconnecting using `valkeyReconnect()`.
These functions return `VALKEY_OK` on success.
On failure, `VALKEY_ERR` is returned and the underlying connection is closed.

To configure these for an asynchronous context (see *Asynchronous API* below), use `ac->c` to get the valkeyContext out of an valkeyAsyncContext.

```C
int valkeyEnableKeepAlive(valkeyContext *c);
int valkeyEnableKeepAliveWithInterval(valkeyContext *c, int interval);
```

Enables TCP keepalive by setting the following socket options (with some variations depending on OS):

* `SO_KEEPALIVE`;
* `TCP_KEEPALIVE` or `TCP_KEEPIDLE`, value configurable using the `interval` parameter, default 15 seconds;
* `TCP_KEEPINTVL` set to 1/3 of `interval`;
* `TCP_KEEPCNT` set to 3.

```C
int valkeySetTcpUserTimeout(valkeyContext *c, unsigned int timeout);
```

Set the `TCP_USER_TIMEOUT` Linux-specific socket option which is as described in the `tcp` man page:

> When the value is greater than 0, it specifies the maximum amount of time in milliseconds that trans mitted data may remain unacknowledged before TCP will forcibly close the corresponding connection and return ETIMEDOUT to the application.
> If the option value is specified as 0, TCP will use the system default.

### Sending commands

There are several ways to issue commands to Valkey. The first that will be introduced is
`valkeyCommand`. This function takes a format similar to printf. In the simplest form,
it is used like this:
```c
reply = valkeyCommand(context, "SET foo bar");
```

The specifier `%s` interpolates a string in the command, and uses `strlen` to
determine the length of the string:
```c
reply = valkeyCommand(context, "SET foo %s", value);
```
When you need to pass binary safe strings in a command, the `%b` specifier can be
used. Together with a pointer to the string, it requires a `size_t` length argument
of the string:
```c
reply = valkeyCommand(context, "SET foo %b", value, (size_t) valuelen);
```
Internally, Libvalkey splits the command in different arguments and will
convert it to the protocol used to communicate with Valkey.
One or more spaces separates arguments, so you can use the specifiers
anywhere in an argument:
```c
reply = valkeyCommand(context, "SET key:%s %s", myid, value);
```

### Using replies

The return value of `valkeyCommand` holds a reply when the command was
successfully executed. When an error occurs, the return value is `NULL` and
the `err` field in the context will be set (see section on **Errors**).
Once an error is returned the context cannot be reused and you should set up
a new connection.

The standard replies that `valkeyCommand` are of the type `valkeyReply`. The
`type` field in the `valkeyReply` should be used to test what kind of reply
was received:

### RESP2

* **`VALKEY_REPLY_STATUS`**:
    * The command replied with a status reply. The status string can be accessed using `reply->str`.
      The length of this string can be accessed using `reply->len`.

* **`VALKEY_REPLY_ERROR`**:
    *  The command replied with an error. The error string can be accessed identical to `VALKEY_REPLY_STATUS`.

* **`VALKEY_REPLY_INTEGER`**:
    * The command replied with an integer. The integer value can be accessed using the
      `reply->integer` field of type `long long`.

* **`VALKEY_REPLY_NIL`**:
    * The command replied with a **nil** object. There is no data to access.

* **`VALKEY_REPLY_STRING`**:
    * A bulk (string) reply. The value of the reply can be accessed using `reply->str`.
      The length of this string can be accessed using `reply->len`.

* **`VALKEY_REPLY_ARRAY`**:
    * A multi bulk reply. The number of elements in the multi bulk reply is stored in
      `reply->elements`. Every element in the multi bulk reply is a `valkeyReply` object as well
      and can be accessed via `reply->element[..index..]`.
      Valkey may reply with nested arrays but this is fully supported.

### RESP3

Libvalkey also supports every new `RESP3` data type which are as follows.  For more information about the protocol see the `RESP3` [specification.](https://github.com/antirez/RESP3/blob/master/spec.md)

* **`VALKEY_REPLY_DOUBLE`**:
    * The command replied with a double-precision floating point number.
      The value is stored as a string in the `str` member, and can be converted with `strtod` or similar.

* **`VALKEY_REPLY_BOOL`**:
    * A boolean true/false reply.
      The value is stored in the `integer` member and will be either `0` or `1`.

* **`VALKEY_REPLY_MAP`**:
    * An array with the added invariant that there will always be an even number of elements.
      The MAP is functionally equivalent to `VALKEY_REPLY_ARRAY` except for the previously mentioned invariant.

* **`VALKEY_REPLY_SET`**:
    * An array response where each entry is unique.
      Like the MAP type, the data is identical to an array response except there are no duplicate values.

* **`VALKEY_REPLY_PUSH`**:
    * An array that can be generated spontaneously by Valkey.
      This array response will always contain at least two subelements.  The first contains the type of `PUSH` message (e.g. `message`, or `invalidate`), and the second being a sub-array with the `PUSH` payload itself.

* **`VALKEY_REPLY_ATTR`**:
    * An array structurally identical to a `MAP` but intended as meta-data about a reply.

* **`VALKEY_REPLY_BIGNUM`**:
    * A string representing an arbitrarily large signed or unsigned integer value.
      The number will be encoded as a string in the `str` member of `valkeyReply`.

* **`VALKEY_REPLY_VERB`**:
    * A verbatim string, intended to be presented to the user without modification.
      The string payload is stored in the `str` member, and type data is stored in the `vtype` member (e.g. `txt` for raw text or `md` for markdown).

Replies should be freed using the `freeReplyObject()` function.
Note that this function will take care of freeing sub-reply objects
contained in arrays and nested arrays, so there is no need for the user to
free the sub replies (it is actually harmful and will corrupt the memory).

**Important:** the current version of valkey (1.0.0) frees replies when the
asynchronous API is used. This means you should not call `freeReplyObject` when
you use this API. The reply is cleaned up by libvalkey _after_ the callback
returns.  We may introduce a flag to make this configurable in future versions of the library.

### Cleaning up

To disconnect and free the context the following function can be used:
```c
void valkeyFree(valkeyContext *c);
```
This function immediately closes the socket and then frees the allocations done in
creating the context.

### Sending commands (continued)

Together with `valkeyCommand`, the function `valkeyCommandArgv` can be used to issue commands.
It has the following prototype:
```c
void *valkeyCommandArgv(valkeyContext *c, int argc, const char **argv, const size_t *argvlen);
```
It takes the number of arguments `argc`, an array of strings `argv` and the lengths of the
arguments `argvlen`. For convenience, `argvlen` may be set to `NULL` and the function will
use `strlen(3)` on every argument to determine its length. Obviously, when any of the arguments
need to be binary safe, the entire array of lengths `argvlen` should be provided.

The return value has the same semantic as `valkeyCommand`.

### Pipelining

To explain how Libvalkey supports pipelining in a blocking connection, there needs to be
understanding of the internal execution flow.

When any of the functions in the `valkeyCommand` family is called, Libvalkey first formats the
command according to the Valkey protocol. The formatted command is then put in the output buffer
of the context. This output buffer is dynamic, so it can hold any number of commands.
After the command is put in the output buffer, `valkeyGetReply` is called. This function has the
following two execution paths:

1. The input buffer is non-empty:
    * Try to parse a single reply from the input buffer and return it
    * If no reply could be parsed, continue at *2*
2. The input buffer is empty:
    * Write the **entire** output buffer to the socket
    * Read from the socket until a single reply could be parsed

The function `valkeyGetReply` is exported as part of the Libvalkey API and can be used when a reply
is expected on the socket. To pipeline commands, the only thing that needs to be done is
filling up the output buffer. For this cause, two commands can be used that are identical
to the `valkeyCommand` family, apart from not returning a reply:
```c
void valkeyAppendCommand(valkeyContext *c, const char *format, ...);
void valkeyAppendCommandArgv(valkeyContext *c, int argc, const char **argv, const size_t *argvlen);
```
After calling either function one or more times, `valkeyGetReply` can be used to receive the
subsequent replies. The return value for this function is either `VALKEY_OK` or `VALKEY_ERR`, where
the latter means an error occurred while reading a reply. Just as with the other commands,
the `err` field in the context can be used to find out what the cause of this error is.

The following examples shows a simple pipeline (resulting in only a single call to `write(2)` and
a single call to `read(2)`):
```c
valkeyReply *reply;
valkeyAppendCommand(context,"SET foo bar");
valkeyAppendCommand(context,"GET foo");
valkeyGetReply(context,(void**)&reply); // reply for SET
freeReplyObject(reply);
valkeyGetReply(context,(void**)&reply); // reply for GET
freeReplyObject(reply);
```
This API can also be used to implement a blocking subscriber:
```c
reply = valkeyCommand(context,"SUBSCRIBE foo");
freeReplyObject(reply);
while(valkeyGetReply(context,(void *)&reply) == VALKEY_OK) {
    // consume message
    freeReplyObject(reply);
}
```
### Errors

When a function call is not successful, depending on the function either `NULL` or `VALKEY_ERR` is
returned. The `err` field inside the context will be non-zero and set to one of the
following constants:

* **`VALKEY_ERR_IO`**:
    There was an I/O error while creating the connection, trying to write
    to the socket or read from the socket. If you included `errno.h` in your
    application, you can use the global `errno` variable to find out what is
    wrong.

* **`VALKEY_ERR_EOF`**:
    The server closed the connection which resulted in an empty read.

* **`VALKEY_ERR_PROTOCOL`**:
    There was an error while parsing the protocol.

* **`VALKEY_ERR_OTHER`**:
    Any other error. Currently, it is only used when a specified hostname to connect
    to cannot be resolved.

In every case, the `errstr` field in the context will be set to hold a string representation
of the error.

## Asynchronous API

Libvalkey comes with an asynchronous API that works easily with any event library.
Examples are bundled that show using Libvalkey with [libev](http://software.schmorp.de/pkg/libev.html)
and [libevent](http://monkey.org/~provos/libevent/).

### Connecting

The function `valkeyAsyncConnect` can be used to establish a non-blocking connection to
Valkey. It returns a pointer to the newly created `valkeyAsyncContext` struct. The `err` field
should be checked after creation to see if there were errors creating the connection.
Because the connection that will be created is non-blocking, the kernel is not able to
instantly return if the specified host and port is able to accept a connection.
In case of error, it is the caller's responsibility to free the context using `valkeyAsyncFree()`

*Note: A `valkeyAsyncContext` is not thread-safe.*

An application function creating a connection might look like this:

```c
void appConnect(myAppData *appData)
{
    valkeyAsyncContext *c = valkeyAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        printf("Error: %s\n", c->errstr);
        // handle error
        valkeyAsyncFree(c);
        c = NULL;
    } else {
        appData->context = c;
        appData->connecting = 1;
        c->data = appData; /* store application pointer for the callbacks */
        valkeyAsyncSetConnectCallback(c, appOnConnect);
        valkeyAsyncSetDisconnectCallback(c, appOnDisconnect);
    }
}

```


The asynchronous context _should_ hold a *connect* callback function that is called when the connection
attempt completes, either successfully or with an error.
It _can_ also hold a *disconnect* callback function that is called when the
connection is disconnected (either because of an error or per user request). Both callbacks should
have the following prototype:
```c
void(const valkeyAsyncContext *c, int status);
```

On a *connect*, the `status` argument is set to `VALKEY_OK` if the connection attempt succeeded.  In this
case, the context is ready to accept commands.  If it is called with `VALKEY_ERR` then the
connection attempt failed. The `err` field in the context can be accessed to find out the cause of the error.
After a failed connection attempt, the context object is automatically freed by the library after calling
the connect callback.  This may be a good point to create a new context and retry the connection.

On a disconnect, the `status` argument is set to `VALKEY_OK` when disconnection was initiated by the
user, or `VALKEY_ERR` when the disconnection was caused by an error. When it is `VALKEY_ERR`, the `err`
field in the context can be accessed to find out the cause of the error.

The context object is always freed after the disconnect callback fired. When a reconnect is needed,
the disconnect callback is a good point to do so.

Setting the connect or disconnect callbacks can only be done once per context. For subsequent calls the
api will return `VALKEY_ERR`. The function to set the callbacks have the following prototype:
```c
/* Alternatively you can use valkeyAsyncSetConnectCallbackNC which will be passed a non-const
   valkeyAsyncContext* on invocation (e.g. allowing writes to the privdata member). */
int valkeyAsyncSetConnectCallback(valkeyAsyncContext *ac, valkeyConnectCallback *fn);
int valkeyAsyncSetDisconnectCallback(valkeyAsyncContext *ac, valkeyDisconnectCallback *fn);
```
`ac->data` may be used to pass user data to both callbacks.  A typical implementation
might look something like this:
```c
void appOnConnect(valkeyAsyncContext *c, int status)
{
    myAppData *appData = (myAppData*)c->data; /* get my application specific context*/
    appData->connecting = 0;
    if (status == VALKEY_OK) {
        appData->connected = 1;
    } else {
        appData->connected = 0;
        appData->err = c->err;
        appData->context = NULL; /* avoid stale pointer when callback returns */
    }
    appAttemptReconnect();
}

void appOnDisconnect(valkeyAsyncContext *c, int status)
{
    myAppData *appData = (myAppData*)c->data; /* get my application specific context*/
    appData->connected = 0;
    appData->err = c->err;
    appData->context = NULL; /* avoid stale pointer when callback returns */
    if (status == VALKEY_OK) {
        appNotifyDisconnectCompleted(mydata);
    } else {
        appNotifyUnexpectedDisconnect(mydata);
        appAttemptReconnect();
    }
}
```

### Sending commands and their callbacks

In an asynchronous context, commands are automatically pipelined due to the nature of an event loop.
Therefore, unlike the synchronous API, there is only a single way to send commands.
Because commands are sent to Valkey asynchronously, issuing a command requires a callback function
that is called when the reply is received. Reply callbacks should have the following prototype:
```c
void(valkeyAsyncContext *c, void *reply, void *privdata);
```
The `privdata` argument can be used to curry arbitrary data to the callback from the point where
the command is initially queued for execution.

The functions that can be used to issue commands in an asynchronous context are:
```c
int valkeyAsyncCommand(
  valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata,
  const char *format, ...);
int valkeyAsyncCommandArgv(
  valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata,
  int argc, const char **argv, const size_t *argvlen);
```
Both functions work like their blocking counterparts. The return value is `VALKEY_OK` when the command
was successfully added to the output buffer and `VALKEY_ERR` otherwise. Example: when the connection
is being disconnected per user-request, no new commands may be added to the output buffer and `VALKEY_ERR` is
returned on calls to the `valkeyAsyncCommand` family.

If the reply for a command with a `NULL` callback is read, it is immediately freed. When the callback
for a command is non-`NULL`, the memory is freed immediately following the callback: the reply is only
valid for the duration of the callback.

All pending callbacks are called with a `NULL` reply when the context encountered an error.

For every command issued, with the exception of **SUBSCRIBE** and **PSUBSCRIBE**, the callback is
called exactly once.  Even if the context object id disconnected or deleted, every pending callback
will be called with a `NULL` reply.

For **SUBSCRIBE** and **PSUBSCRIBE**, the callbacks may be called repeatedly until an `unsubscribe`
message arrives.  This will be the last invocation of the callback. In case of error, the callbacks
may receive a final `NULL` reply instead.

### Disconnecting

An asynchronous connection can be terminated using:
```c
void valkeyAsyncDisconnect(valkeyAsyncContext *ac);
```
When this function is called, the connection is **not** immediately terminated. Instead, new
commands are no longer accepted and the connection is only terminated when all pending commands
have been written to the socket, their respective replies have been read and their respective
callbacks have been executed. After this, the disconnection callback is executed with the
`VALKEY_OK` status and the context object is freed.

The connection can be forcefully disconnected using
```c
void valkeyAsyncFree(valkeyAsyncContext *ac);
```
In this case, nothing more is written to the socket, all pending callbacks are called with a `NULL`
reply and the disconnection callback is called with `VALKEY_OK`, after which the context object
is freed.


### Hooking it up to event library *X*

There are a few hooks that need to be set on the context object after it is created.
See the `adapters/` directory for bindings to *libev* and *libevent*.

## Reply parsing API

Libvalkey comes with a reply parsing API that makes it easy for writing higher
level language bindings.

The reply parsing API consists of the following functions:
```c
valkeyReader *valkeyReaderCreate(void);
void valkeyReaderFree(valkeyReader *reader);
int valkeyReaderFeed(valkeyReader *reader, const char *buf, size_t len);
int valkeyReaderGetReply(valkeyReader *reader, void **reply);
```
The same set of functions are used internally by libvalkey when creating a
normal Valkey context, the above API just exposes it to the user for a direct
usage.

### Usage

The function `valkeyReaderCreate` creates a `valkeyReader` structure that holds a
buffer with unparsed data and state for the protocol parser.

Incoming data -- most likely from a socket -- can be placed in the internal
buffer of the `valkeyReader` using `valkeyReaderFeed`. This function will make a
copy of the buffer pointed to by `buf` for `len` bytes. This data is parsed
when `valkeyReaderGetReply` is called. This function returns an integer status
and a reply object (as described above) via `void **reply`. The returned status
can be either `VALKEY_OK` or `VALKEY_ERR`, where the latter means something went
wrong (either a protocol error, or an out of memory error).

The parser limits the level of nesting for multi bulk payloads to 7. If the
multi bulk nesting level is higher than this, the parser returns an error.

### Customizing replies

The function `valkeyReaderGetReply` creates `valkeyReply` and makes the function
argument `reply` point to the created `valkeyReply` variable. For instance, if
the response of type `VALKEY_REPLY_STATUS` then the `str` field of `valkeyReply`
will hold the status as a vanilla C string. However, the functions that are
responsible for creating instances of the `valkeyReply` can be customized by
setting the `fn` field on the `valkeyReader` struct. This should be done
immediately after creating the `valkeyReader`.

For example, [hiredis-rb](https://github.com/pietern/hiredis-rb/blob/master/ext/hiredis_ext/reader.c)
uses customized reply object functions to create Ruby objects.

### Reader max buffer

Both when using the Reader API directly or when using it indirectly via a
normal Valkey context, the valkeyReader structure uses a buffer in order to
accumulate data from the server.
Usually this buffer is destroyed when it is empty and is larger than 16
KiB in order to avoid wasting memory in unused buffers

However when working with very big payloads destroying the buffer may slow
down performances considerably, so it is possible to modify the max size of
an idle buffer changing the value of the `maxbuf` field of the reader structure
to the desired value. The special value of 0 means that there is no maximum
value for an idle buffer, so the buffer will never get freed.

For instance if you have a normal Valkey context you can set the maximum idle
buffer to zero (unlimited) just with:
```c
context->reader->maxbuf = 0;
```
This should be done only in order to maximize performances when working with
large payloads. The context should be set back to `VALKEY_READER_MAX_BUF` again
as soon as possible in order to prevent allocation of useless memory.

### Reader max array elements

By default the libvalkey reply parser sets the maximum number of multi-bulk elements
to 2^32 - 1 or 4,294,967,295 entries.  If you need to process multi-bulk replies
with more than this many elements you can set the value higher or to zero, meaning
unlimited with:
```c
context->reader->maxelements = 0;
```

## SSL/TLS Support

### Building

SSL/TLS support is not built by default and requires an explicit flag:

    make USE_SSL=1

This requires OpenSSL development package (e.g. including header files to be
available.

When enabled, SSL/TLS support is built into extra `libvalkey_ssl.a` and
`libvalkey_ssl.so` static/dynamic libraries. This leaves the original libraries
unaffected so no additional dependencies are introduced.

### Using it

First, you'll need to make sure you include the SSL header file:

```c
#include <valkey/valkey.h>
#include <valkey/valkey_ssl.h>
```

You will also need to link against `libhivalkey_ssl`, **in addition** to
`libvalkey` and add `-lssl -lcrypto` to satisfy its dependencies.

Libvalkey implements SSL/TLS on top of its normal `valkeyContext` or
`valkeyAsyncContext`, so you will need to establish a connection first and then
initiate an SSL/TLS handshake.

#### Libvalkey OpenSSL Wrappers

Before Libvalkey can negotiate an SSL/TLS connection, it is necessary to
initialize OpenSSL and create a context. You can do that in two ways:

1. Work directly with the OpenSSL API to initialize the library's global context
   and create `SSL_CTX *` and `SSL *` contexts. With an `SSL *` object you can
   call `valkeyInitiateSSL()`.
2. Work with a set of Libvalkey-provided wrappers around OpenSSL, create a
   `valkeySSLContext` object to hold configuration and use
   `valkeyInitiateSSLWithContext()` to initiate the SSL/TLS handshake.

```c
/* An Libvalkey SSL context. It holds SSL configuration and can be reused across
 * many contexts.
 */
valkeySSLContext *ssl_context;

/* An error variable to indicate what went wrong, if the context fails to
 * initialize.
 */
valkeySSLContextError ssl_error = VALKEY_SSL_CTX_NONE;

/* Initialize global OpenSSL state.
 *
 * You should call this only once when your app initializes, and only if
 * you don't explicitly or implicitly initialize OpenSSL it elsewhere.
 */
valkeyInitOpenSSL();

/* Create SSL context */
ssl_context = valkeyCreateSSLContext(
    "cacertbundle.crt",     /* File name of trusted CA/ca bundle file, optional */
    "/path/to/certs",       /* Path of trusted certificates, optional */
    "client_cert.pem",      /* File name of client certificate file, optional */
    "client_key.pem",       /* File name of client private key, optional */
    "valkey.mydomain.com",   /* Server name to request (SNI), optional */
    &ssl_error);

if(ssl_context == NULL || ssl_error != VALKEY_SSL_CTX_NONE) {
    /* Handle error and abort... */
    /* e.g.
    printf("SSL error: %s\n",
        (ssl_error != VALKEY_SSL_CTX_NONE) ?
            valkeySSLContextGetError(ssl_error) : "Unknown error");
    // Abort
    */
}

/* Create Valkey context and establish connection */
c = valkeyConnect("localhost", 6443);
if (c == NULL || c->err) {
    /* Handle error and abort... */
}

/* Negotiate SSL/TLS */
if (valkeyInitiateSSLWithContext(c, ssl_context) != VALKEY_OK) {
    /* Handle error, in c->err / c->errstr */
}
```

## RESP3 PUSH replies
`RESP3` introduced PUSH replies with the reply-type `>`.  These messages are generated spontaneously and can arrive at any time, so must be handled using callbacks.

### Default behavior
Libvalkey installs handlers on `valkeyContext` and `valkeyAsyncContext` by default, which will intercept and free any PUSH replies detected.  This means existing code will work as-is after upgrading to to `RESP3`.

### Custom PUSH handler prototypes
The callback prototypes differ between `valkeyContext` and `valkeyAsyncContext`.

#### valkeyContext
```c
void my_push_handler(void *privdata, void *reply) {
    /* Handle the reply */

    /* Note: We need to free the reply in our custom handler for
             blocking contexts.  This lets us keep the reply if
             we want. */
    freeReplyObject(reply);
}
```

#### valkeyAsyncContext
```c
void my_async_push_handler(valkeyAsyncContext *ac, void *reply) {
    /* Handle the reply */

    /* Note:  Because async libvalkey always frees replies, you should
              not call freeReplyObject in an async push callback. */
}
```

### Installing a custom handler
There are two ways to set your own PUSH handlers.

1. Set `push_cb` or `async_push_cb` in the `valkeyOptions` struct and connect with `valkeyConnectWithOptions` or `valkeyAsyncConnectWithOptions`.
    ```c
    valkeyOptions = {0};
    VALKEY_OPTIONS_SET_TCP(&options, "127.0.0.1", 6379);
    options->push_cb = my_push_handler;
    valkeyContext *context = valkeyConnectWithOptions(&options);
    ```
2.  Call `valkeySetPushCallback` or `valkeyAsyncSetPushCallback` on a connected context.
    ```c
    valkeyContext *context = valkeyConnect("127.0.0.1", 6379);
    valkeySetPushCallback(context, my_push_handler);
    ```

    _Note `valkeySetPushCallback` and `valkeyAsyncSetPushCallback` both return any currently configured handler,  making it easy to override and then return to the old value._

### Specifying no handler
If you have a unique use-case where you don't want libvalkey to automatically intercept and free PUSH replies, you will want to configure no handler at all.  This can be done in two ways.
1.  Set the `VALKEY_OPT_NO_PUSH_AUTOFREE` flag in `valkeyOptions` and leave the callback function pointer `NULL`.
    ```c
    valkeyOptions = {0};
    VALKEY_OPTIONS_SET_TCP(&options, "127.0.0.1", 6379);
    options->options |= VALKEY_OPT_NO_PUSH_AUTOFREE;
    valkeyContext *context = valkeyConnectWithOptions(&options);
    ```
3.  Call `valkeySetPushCallback` with `NULL` once connected.
    ```c
    valkeyContext *context = valkeyConnect("127.0.0.1", 6379);
    valkeySetPushCallback(context, NULL);
    ```

    _Note:  With no handler configured, calls to `valkeyCommand` may generate more than one reply, so this strategy is only applicable when there's some kind of blocking `valkeyGetReply()` loop (e.g. `MONITOR` or `SUBSCRIBE` workloads)._

## Allocator injection

Libvalkey uses a pass-thru structure of function pointers defined in [alloc.h](https://github.com/valkey-io/libvalkey/blob/main/alloc.h) that contain the currently configured allocation and deallocation functions.  By default they just point to libc (`malloc`, `calloc`, `realloc`, and `strdup`).

### Overriding

One can override the allocators like so:

```c
valkeyAllocFuncs myfuncs = {
    .mallocFn = my_malloc,
    .callocFn = my_calloc,
    .reallocFn = my_realloc,
    .strdupFn = my_strdup,
    .freeFn = my_free,
};

// Override allocators (function returns current allocators if needed)
valkeyAllocFuncs orig = valkeyAllocators(&myfuncs);
```

To reset the allocators to their default libc function simply call:

```c
valkeyResetAllocators();
```

## AUTHORS

Salvatore Sanfilippo (antirez at gmail),\
Pieter Noordhuis (pcnoordhuis at gmail)\
Michael Grunder (michael dot grunder at gmail)

_Libvalkey is released under the BSD license._
