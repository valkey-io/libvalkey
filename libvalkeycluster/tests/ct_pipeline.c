#include "adapters/libevent.h"
#include "valkeycluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_NODE "127.0.0.1:7000"

// Test of two pipelines using sync API
void test_pipeline(void) {
    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);

    int status;
    status = valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterConnect2(cc);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterAppendCommand(cc, "SET foo one");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    status = valkeyClusterAppendCommand(cc, "SET bar two");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    status = valkeyClusterAppendCommand(cc, "GET foo");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    status = valkeyClusterAppendCommand(cc, "GET bar");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    status = valkeyClusterAppendCommand(cc, "SUNION a b");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    valkeyReply *reply;
    valkeyClusterGetReply(cc, (void *)&reply); // reply for: SET foo one
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply); // reply for: SET bar two
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply); // reply for: GET foo
    CHECK_REPLY_STR(cc, reply, "one");
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply); // reply for: GET bar
    CHECK_REPLY_STR(cc, reply, "two");
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply); // reply for: SUNION a b
    CHECK_REPLY_ERROR(cc, reply, "CROSSSLOT");
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

// Test of pipelines containing multi-node commands
void test_pipeline_with_multinode_commands(void) {
    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);

    int status;
    status = valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterConnect2(cc);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterAppendCommand(cc, "MSET key1 Hello key2 World key3 !");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterAppendCommand(cc, "MGET key1 key2 key3");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    valkeyReply *reply;
    valkeyClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY_ARRAY(cc, reply, 3);
    CHECK_REPLY_STR(cc, reply->element[0], "Hello");
    CHECK_REPLY_STR(cc, reply->element[1], "World");
    CHECK_REPLY_STR(cc, reply->element[2], "!");
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

//------------------------------------------------------------------------------
// Async API
//------------------------------------------------------------------------------

typedef struct ExpectedResult {
    int type;
    char *str;
    bool disconnect;
} ExpectedResult;

// Callback for Valkey connects and disconnects
void callbackExpectOk(const valkeyAsyncContext *ac, int status) {
    UNUSED(ac);
    assert(status == VALKEY_OK);
}

// Callback for async commands, verifies the valkeyReply
void commandCallback(valkeyClusterAsyncContext *cc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    ExpectedResult *expect = (ExpectedResult *)privdata;
    assert(reply != NULL);
    assert(reply->type == expect->type);
    assert(strcmp(reply->str, expect->str) == 0);

    if (expect->disconnect) {
        valkeyClusterAsyncDisconnect(cc);
    }
}

// Test of two pipelines using async API
// In an asynchronous context, commands are automatically pipelined due to the
// nature of an event loop. Therefore, unlike the synchronous API, there is only
// a single way to send commands.
void test_async_pipeline(void) {
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);
    valkeyClusterAsyncSetConnectCallback(acc, callbackExpectOk);
    valkeyClusterAsyncSetDisconnectCallback(acc, callbackExpectOk);
    valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE);

    int status;
    status = valkeyClusterConnect2(acc->cc);
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    struct event_base *base = event_base_new();
    status = valkeyClusterLibeventAttach(acc, base);
    assert(status == VALKEY_OK);

    ExpectedResult r1 = {.type = VALKEY_REPLY_STATUS, .str = "OK"};
    status = valkeyClusterAsyncCommand(acc, commandCallback, &r1, "SET foo six");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    ExpectedResult r2 = {.type = VALKEY_REPLY_STATUS, .str = "OK"};
    status = valkeyClusterAsyncCommand(acc, commandCallback, &r2, "SET bar ten");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    ExpectedResult r3 = {.type = VALKEY_REPLY_STRING, .str = "six"};
    status = valkeyClusterAsyncCommand(acc, commandCallback, &r3, "GET foo");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    ExpectedResult r4 = {.type = VALKEY_REPLY_STRING, .str = "ten"};
    status = valkeyClusterAsyncCommand(acc, commandCallback, &r4, "GET bar");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    ExpectedResult r5 = {
        .type = VALKEY_REPLY_ERROR,
        .str = "CROSSSLOT Keys in request don't hash to the same slot",
        .disconnect = true};
    status = valkeyClusterAsyncCommand(acc, commandCallback, &r5, "SUNION a b");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

int main(void) {

    test_pipeline();
    test_pipeline_with_multinode_commands();

    test_async_pipeline();
    // Asynchronous API does not support multi-key commands

    return 0;
}
