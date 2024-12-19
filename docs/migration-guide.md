# Migration guide

Libvalkey can replace both libraries `hiredis` and `hiredis-cluster`.
This guide highlights which APIs that have changed and what you need to do when migrating to libvalkey.

The general actions needed are:

* Replace the prefix `redis` with `valkey` in API usages.
* Replace the term `SSL` with `TLS` in API usages for secure communication.
* Update include paths depending on your previous installation.
  All `libvalkey` headers are now found under `include/valkey/`.
* Update used build options, e.g. `USE_TLS` replaces `USE_SSL`.

## Migrating from `hiredis` v1.2.0

The type `sds` is removed from the public API.

### Renamed API functions

* `redisAsyncSetConnectCallbackNC` is renamed to `valkeyAsyncSetConnectCallback`.

### Removed API functions

* `redisFormatSdsCommandArgv` removed from API. Can be replaced with `valkeyFormatCommandArgv`.
* `redisFreeSdsCommand` removed since the `sds` type is for internal use only.
* `redisAsyncSetConnectCallback` is removed, but can be replaced with `valkeyAsyncSetConnectCallback` which accepts the non-const callback function prototype.

## Migrating from `hiredis-cluster` 0.14.0

### Renamed API functions

* `ctx_get_by_node` is renamed to `valkeyClusterGetValkeyContext`.
* `actx_get_by_node` is renamed to `valkeyClusterGetValkeyAsyncContext`.
* `redisClusterAsyncSetConnectCallbackNC` is renamed to `valkeyClusterAsyncSetConnectCallback`.

### Renamed API defines

* `REDIS_ROLE_NULL` is renamed to `VALKEY_ROLE_UNKNOWN`.
* `REDIS_ROLE_MASTER` is renamed to `VALKEY_ROLE_PRIMARY`.
* `REDIS_ROLE_SLAVE` is renamed to `VALKEY_ROLE_REPLICA`.

### Removed API functions

* `redisClusterSetMaxRedirect` removed and replaced with `valkeyClusterSetOptionMaxRetry`.
* `redisClusterSetOptionAddNode` removed and replaced with `valkeyClusterSetOptionAddNodes`.
  (Note the "s" in the end of the function name.)
* `redisClusterSetOptionConnectBlock` removed since it was deprecated.
* `redisClusterSetOptionConnectNonBlock` removed since it was deprecated.
* `parse_cluster_nodes` removed from API, for internal use only.
* `parse_cluster_slots` removed from API, for internal use only.
* `redisClutserAsyncSetConnectCallback` is removed, but can be replaced with `valkeyClusterAsyncSetConnectCallback` which accepts the non-const callback function prototype.

### Removed support for splitting multi-key commands per slot

Since old days (from `hiredis-vip`) there has been support for sending some commands with multiple keys that covers multiple slots.
The client would split the command into multiple commands and send to each node handling each slot.
This was unnecessary complex and broke any expectations of atomicity.
Commands affected are `DEL`, `EXISTS`, `MGET` and `MSET`.

_Proposed action:_

Partition the keys by slot using `valkeyClusterGetSlotByKey` before sending affected commands.
Construct new commands when needed and send them using multiple calls to `valkeyClusterCommand` or equivalent.
