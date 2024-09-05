# Cluster API documentation

This document describes using `libvalkey` in cluster mode, including an overview of the synchronous and asynchronous APIs.
It is not intended as a complete reference. For that it's always best to refer to the source code.

## Table of Contents

- [Synchronous API](#synchronous-api)
  - [Connecting](#connecting)
  - [Executing commands](#executing-commands)
  - [Executing commands on a specific node](#executing-commands-on-a-specific-node)
  - [Disconnecting/cleanup](#disconnecting-cleanup)
  - [Pipelining](#pipelining)
  - [Events](#events)
- [Asynchronous API](#asynchronous-api)
  - [Connecting](#connecting-1)
  - [Executing commands](#executing-commands-1)
  - [Executing commands on a specific node](#executing-commands-on-a-specific-node-1)
  - [Disconnecting/cleanup](#disconnecting-cleanup-1)
  - [Events](#events-1)
- [Miscellaneous](#miscellaneous)
  - [Cluster node iterator](#cluster-node-iterator)
  - [Extend the list of supported commands](#extend-the-list-of-supported-commands)
  - [Random number generator](#random-number-generator)

## Synchronous API

### Connecting

There are a variety of ways to setup and connect to a cluster.
The most basic alternative lacks many options, but can be enough for some use cases.

```c
valkeyClusterContext *cc = valkeyClusterConnect("127.0.0.1:6379", VALKEYCLUSTER_FLAG_NULL);
if (cc == NULL || cc->err) {
    fprintf(stderr, "Error: %s\n", cc ? cc->errstr : "OOM");
}
```

When connecting to a cluster, libvalkey will return `NULL` in the event that we can't allocate the context, and set the `err` member if we can connect but there are issues.
So when connecting it's simple to handle error states.

To be able to set options before any connection attempt is performed a `valkeyClusterContext` can be created using `valkeyClusterContextInit`.
The context is where the known cluster state and the options are kept, which can be configured using functions like
`valkeyClusterSetOptionAddNodes` to add one or many cluster node addresses,
or `valkeyClusterSetOptionUsername` together with `valkeyClusterSetOptionPassword` to configure authentication and so on.
See [`include/valkey/cluster.h`](../include/valkey/cluster.h) for more details.

The function `valkeyClusterConnect2` is provided to connect using the prepared `valkeyClusterContext`.

```c
valkeyClusterContext *cc = valkeyClusterContextInit();
valkeyClusterSetOptionAddNodes(cc, "127.0.0.1:6379,127.0.0.1:6380");
int status = valkeyClusterConnect2(cc);
if (status == VALKEY_ERR) {
    printf("Error: %s\n", cc->errstr);
    // handle error
}
```

### Executing commands

The primary command interface is a `printf`-like function that takes a format string along with a variable number of arguments.
This will construct a `RESP` command and deliver it to the correct node in the cluster.

```c
valkeyReply *reply = valkeyClusterCommand(cc, "SET %s %d", "counter", 42);

if (reply == NULL) {
    fprintf(stderr, "Communication error: %s\n", cc->err ? cc->errstr : "Unknown error");
} else if (reply->type == VALKEY_REPLY_ERROR) {
    fprintf(stderr, "Error response from node: %s\n", reply->str);
} else {
    // Handle reply..
}
freeReplyObject(reply);
```

Commands will be sent to the cluster node that the client perceives handling the given key.
If the cluster topology has changed the Valkey node might respond with a redirection error which the client will handle, update its slot map and resend the command to correct node.
The reply will in this case arrive from the correct node.

If a node is unreachable, for example if the command times out or if the connect times out, it can indicate that there has been a failover and the node is no longer part of the cluster.
In this case, `valkeyClusterCommand` returns NULL and sets `err` and `errstr` on the cluster context, but additionally, libvalkey schedules a slot map update to be performed when the next command is sent.
That means that if you try the same command again, there is a good chance the command will be sent to another node and the command may succeed.

### Executing commands on a specific node

When there is a need to send commands to a specific node, the following low-level API can be used.

```c
valkeyReply *reply = valkeyClusterCommandToNode(cc, node, "DBSIZE");
```

This function handles `printf`-like arguments similar to `valkeyClusterCommand()`, but will only attempt to send the command to the given node and will not perform redirects or retries.

If the command times out or the connection to the node fails, a slot map update is scheduled to be performed when the next command is sent.
`valkeyClusterCommandToNode` also performs a slot map update if it has previously been scheduled.

### Disconnecting/cleanup

To disconnect and free the context the following function can be used:

```c
valkeyClusterFree(cc);
```

This function closes the sockets and deallocates the context.

### Pipelining

For pipelined commands, every command is simply appended to the output buffer but not delivered to the server, until you attempt to read the first response, at which point the entire buffer will be delivered.

The `valkeyClusterAppendCommand` function can be used to append a command, which is identical to the `valkeyClusterCommand` family, apart from not returning a reply.
After calling an append function `valkeyClusterGetReply` can be used to receive the subsequent replies.

The following example shows a simple cluster pipeline.

```c
if (valkeyClusterAppendCommand(cc, "SET foo bar") != VALKEY_OK) {
    fprintf(stderr, "Error appending command: %s\n", cc->errstr);
    exit(1);
}
if (valkeyClusterAppendCommand(cc, "GET foo") != VALKEY_OK) {
    fprintf(stderr, "Error appending command: %s\n", cc->errstr);
    exit(1);
}

valkeyReply *reply;
if (valkeyClusterGetReply(cc,&reply) != VALKEY_OK) {
        fprintf(stderr, "Error reading reply %zu: %s\n", i, c->errstr);
        exit(1);
}
// Handle the reply for SET here.
freeReplyObject(reply);

if (valkeyClusterGetReply(cc,&reply) != VALKEY_OK) {
        fprintf(stderr, "Error reading reply %zu: %s\n", i, c->errstr);
        exit(1);
}
// Handle the reply for GET here.
freeReplyObject(reply);
```

### Events

#### Events per cluster context

There is a hook to get notified when certain events occur.

```c
int valkeyClusterSetEventCallback(valkeyClusterContext *cc,
                                  void(fn)(const valkeyClusterContext *cc, int event,
                                           void *privdata),
                                  void *privdata);
```

The callback is called with `event` set to one of the following values:

* `VALKEYCLUSTER_EVENT_SLOTMAP_UPDATED` when the slot mapping has been updated;
* `VALKEYCLUSTER_EVENT_READY` when the slot mapping has been fetched for the first
  time and the client is ready to accept commands, useful when initiating the
  client with `valkeyClusterAsyncConnect2()` where a client is not immediately
  ready after a successful call;
* `VALKEYCLUSTER_EVENT_FREE_CONTEXT` when the cluster context is being freed, so
  that the user can free the event `privdata`.

#### Events per connection

There is a hook to get notified about connect and reconnect attempts.
This is useful for applying socket options or access endpoint information for a connection to a particular node.
The callback is registered using the following function:

```c
int valkeyClusterSetConnectCallback(valkeyClusterContext *cc,
                                    void(fn)(const valkeyContext *c, int status));
```

The callback is called just after connect, before TLS handshake and authentication.

On successful connection, `status` is set to `VALKEY_OK` and the `valkeyContext` can be used, for example,
to see which IP and port it's connected to or to set socket options directly on the file descriptor which can be accessed as `c->fd`.

On failed connection attempt, this callback is called with `status` set to `VALKEY_ERR`.
The `err` field in the `valkeyContext` can be used to find out the cause of the error.

## Asynchronous API

### Connecting

There are two alternative ways to initiate a cluster client, which also determines how the client behaves during the initial connect.

The first alternative is to use the function `valkeyClusterAsyncConnect`, which initially connects to the cluster in a blocking fashion and waits for the slot map before returning.
Any command sent by the user thereafter will create a new non-blocking connection, unless a non-blocking connection already exists to the destination.
The function returns a pointer to a newly created `valkeyClusterAsyncContext` struct and its `err` field should be checked to make sure the initial slot map update was successful.

```c
// Insufficient error handling for brevity.
valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnect("127.0.0.1:6379", VALKEYCLUSTER_FLAG_NULL);
if (acc->err) {
    printf("error: %s\n", acc->errstr);
    exit(1);
}

// Attach an event engine. In this example we use libevent.
struct event_base *base = event_base_new();
valkeyClusterLibeventAttach(acc, base);
```

The second alternative is to use `valkeyClusterAsyncContextInit` and `valkeyClusterAsyncConnect2` which avoids the initial blocking connect.
This connection alternative requires an attached event engine when `valkeyClusterAsyncConnect2` is called, but the connect and the initial slot map update is done in a non-blocking fashion.

This means that commands sent directly after `valkeyClusterAsyncConnect2` may fail because the initial slot map has not yet been retrieved and the client doesn't know which cluster node to send the command to.
You may use the [`eventCallback`](#events-per-cluster-context-1) to be notified when the slot map is updated and the client is ready to accept commands.
A crude example of using the `eventCallback` can be found in [this test case](../tests/ct_async.c).

```c
// Insufficient error handling for brevity.
valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();

// Add a cluster node address for the initial connect.
valkeyClusterSetOptionAddNodes(acc->cc, "127.0.0.1:6379");

// Attach an event engine. In this example we use libevent.
struct event_base *base = event_base_new();
valkeyClusterLibeventAttach(acc, base);

if (valkeyClusterAsyncConnect2(acc) != VALKEY_OK) {
    printf("error: %s\n", acc->errstr);
    exit(1);
}
```

### Executing commands

Executing commands in an asynchronous context work similarly to the synchronous context, except that you can pass a callback that will be invoked when the reply is received.

```c
int status = valkeyClusterAsyncCommand(cc, commandCallback, privdata,
                                       "SET %s %s", "key", "value");
```

The return value is `VALKEY_OK` when the command was successfully added to the output buffer and `VALKEY_ERR` otherwise.
When the connection is being disconnected per user-request, no new commands may be added to the output buffer and `VALKEY_ERR` is returned.

Commands will be sent to the cluster node that the client perceives handling the given key.
If the cluster topology has changed the Valkey node might respond with a redirection error which the client will handle, update its slot map and resend the command to correct node.
The reply will in this case arrive from the correct node.

The reply callback, that is called when the reply is received, should have the following prototype:

```c
void(valkeyClusterAsyncContext *acc, void *reply, void *privdata);
```

The `privdata` argument can be used to carry arbitrary data to the callback.

All pending callbacks are called with a `NULL` reply when the context encountered an error.

### Executing commands on a specific node

When there is a need to send commands to a specific node, the following low-level API can be used.

```c
status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback, privdata, "DBSIZE");
```

This functions will only attempt to send the command to a specific node and will not perform redirects or retries, but communication errors will trigger a slot map update just like the commonly used API.


### Disconnecting/cleanup

Asynchronous cluster connections can be terminated using:

```c
valkeyClusterAsyncDisconnect(acc);
```

When this function is called, connections are **not** immediately terminated.
Instead, new commands are no longer accepted and connections are only terminated when all pending commands have been written to a socket, their respective replies have been read and their respective callbacks have been executed.
After this, the disconnection callback is executed with the `VALKEY_OK` status and the context object is freed.

### Events

#### Events per cluster context

Use [`valkeyClusterSetEventCallback`](#events-per-cluster-context) with `acc->cc` as the context to get notified when certain events occur.

#### Events per connection

Because the connections that will be created are non-blocking, the kernel is not able to instantly return if the specified host and port is able to accept a connection.
Instead, use a connect callback to be notified when a connection is established or failed.
Similarly, a disconnect callback can be used to be notified about a disconnected connection (either because of an error or per user request).
The callbacks are installed using the following functions:

```c
status = valkeyClusterAsyncSetConnectCallback(acc, callbackFn);
status = valkeyClusterAsyncSetDisonnectCallback(acc, callbackFn);
```

The callback functions should have the following prototype, aliased to `valkeyConnectCallback`:

```c
void(const valkeyAsyncContext *ac, int status);
```

Alternatively, you set a connect callback that will be passed a non-const `valkeyAsyncContext*` on invocation (e.g. to be able to set a push callback on it).

```c
status = valkeyClusterAsyncSetConnectCallbackNC(acc, nonConstCallbackFn);
```

The callback function should have the following prototype, aliased to `valkeyConnectCallbackNC`:
```c
void(valkeyAsyncContext *ac, int status);
```

On a connection attempt, the `status` argument is set to `VALKEY_OK` when the connection was successful.
The file description of the connection socket can be retrieved from a `valkeyAsyncContext` as `ac->c->fd`.
On a disconnect, the `status` argument is set to `VALKEY_OK` when disconnection was initiated by the user, or `VALKEY_ERR` when the disconnection was caused by an error.
When it is `VALKEY_ERR`, the `err` field in the context can be accessed to find out the cause of the error.

You don't need to reconnect in the disconnect callback.
libvalkey will reconnect by itself when the next command is handled.

Setting the connect and disconnect callbacks can only be done once per context.
For subsequent calls it will return `VALKEY_ERR`.

## Miscellaneous

### Cluster node iterator

A `valkeyClusterNodeIterator` can be used to iterate on all known master nodes in a cluster context.
First it needs to be initiated using `valkeyClusterInitNodeIterator` and then you can repeatedly call `valkeyClusterNodeNext` to get the next node from the iterator.

```c
valkeyClusterNodeIterator ni;
valkeyClusterInitNodeIterator(&ni, cc);

valkeyClusterNode *node;
while ((node = valkeyClusterNodeNext(&ni)) != NULL) {
   valkeyReply *reply = valkeyClusterCommandToNode(cc, node, "DBSIZE");
   // Handle reply..
}
```

The iterator will handle changes due to slot map updates by restarting the iteration, but on the new set of master nodes.
There is no bookkeeping for already iterated nodes when a restart is triggered, which means that a node can be iterated over more than once depending on when the slot map update happened and the change of cluster nodes.

Note that when `valkeyClusterCommandToNode` is called, a slot map update can happen if it has been scheduled by the previous command, for example if the previous call to `valkeyClusterCommandToNode` timed out or the node wasn't reachable.

To detect when the slot map has been updated, you can check if the slot map version (`iter.route_version`) is equal to the current cluster context's slot map version (`cc->route_version`).
If it isn't, it means that the slot map has been updated and the iterator will restart itself at the next call to `valkeyClusterNodeNext`.

Another way to detect that the slot map has been updated is to [register an event callback](#events-per-cluster-context) and look for the event `VALKEYCLUSTER_EVENT_SLOTMAP_UPDATED`.

### Extend the list of supported commands

The list of commands and the position of the first key in the command line is defined in [`src/cmddef.h`](../src/cmddef.h) which is included in this repository.
It has been generated using the `JSON` files describing the syntax of each command in the Valkey repository, which makes sure we support all commands in Valkey, at least in terms of cluster routing.
To add support for custom commands defined in modules, you can regenerate `cmddef.h` using the script [`gencommands.py`](../script/gencommands.py).
Use the `JSON` files from Valkey and any additional files on the same format as arguments to the script.
For details, see the comments inside `gencommands.py`.

### Random number generator

This library uses [random()](https://linux.die.net/man/3/random) while selecting a node used for requesting the cluster topology (slot map).
A user should seed the random number generator using [`srandom()`](https://linux.die.net/man/3/srandom) to get less predictability in the node selection.
