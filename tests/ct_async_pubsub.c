#define _XOPEN_SOURCE 600
#include "adapters/libuv.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <string.h>

static char channelA[] = "{libvalkey-test}:a";
static const char channelB[] = "{libvalkey-test}:b";
static char otherChannel[] = "other-slot";

typedef struct {
    const char *channel;
    int reply_type;
    int acks, messages, unsubs, disconnects;
} subscription;

static void onSubscription(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    subscription *sub = privdata;
    valkeyReply *reply = r;
    if (!reply) {
        sub->disconnects++;
        return;
    }

    assert(reply->type == sub->reply_type && reply->elements == 3);
    const char *kind = reply->element[0]->str;
    const char *channel = reply->element[1]->str;
    if (!strcmp(kind, "ssubscribe")) {
        sub->acks++;
    } else if (!strcmp(kind, "smessage")) {
        assert(!strcmp(channel, sub->channel));
        assert(!strcmp(reply->element[2]->str, "payload"));
        sub->messages++;
        if (!strcmp(channel, channelA)) {
            assert(valkeyClusterAsyncCommand(acc, NULL, NULL, "SUNSUBSCRIBE %s", channel) == VALKEY_OK);
        }
    } else {
        assert(!strcmp(kind, "sunsubscribe") && !strcmp(channel, channelA));
        assert(!strcmp(channel, sub->channel));
        sub->unsubs++;
        valkeyClusterAsyncDisconnect(acc);
    }
}

static void onRejectedSubscribe(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    int *errors = privdata;
    valkeyReply *reply = r;
    CHECK_REPLY_ERROR(acc, reply, "CROSSSLOT");
    (*errors)++;

    /* Publish through a separate (synchronous) connection on the same shard. */
    const char *channels[] = {channelB, channelA};
    for (size_t i = 0; i < 2; i++) {
        reply = valkeyClusterCommand(&acc->cc, "SPUBLISH %s payload", channels[i]);
        CHECK_REPLY_INT((&acc->cc), reply, 1);
        freeReplyObject(reply);
    }
}

static void run(const char *seed, int resp) {
    uv_loop_t loop;
    assert(uv_loop_init(&loop) == 0);
    valkeyClusterOptions options = {0};
    options.initial_nodes = seed;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    valkeyClusterOptionsUseLibuv(&options, &loop);
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    assert(acc && !acc->err);

    valkeyClusterNode *node = valkeyClusterGetNodeByKey(&acc->cc, channelA);
    assert(node);
    assert(valkeyClusterAsyncCommandToNode(acc, node, NULL, NULL, "HELLO %d", resp) == VALKEY_OK);

    int type = resp == 3 ? VALKEY_REPLY_PUSH : VALKEY_REPLY_ARRAY;
    subscription original = {channelB, type, 0, 0, 0, 0};
    subscription replacement = {channelA, type, 0, 0, 0, 0};
    int errors = 0;
    assert(valkeyClusterAsyncCommand(acc, onSubscription, &original, "SSUBSCRIBE %s %s", channelA, channelB) == VALKEY_OK);
    assert(valkeyClusterAsyncCommandToNode(acc, node, onSubscription, &replacement, "SSUBSCRIBE %s", channelA) == VALKEY_OK);
    assert(valkeyClusterAsyncCommand(acc, onRejectedSubscribe, &errors, "SSUBSCRIBE %s %s", channelA, otherChannel) == VALKEY_OK);
    uv_run(&loop, UV_RUN_DEFAULT);
    assert(errors == 1);
    assert(original.acks == 2 && original.messages == 1 && original.unsubs == 0 && original.disconnects == 1);
    assert(replacement.acks == 1 && replacement.messages == 1 && replacement.unsubs == 1 && replacement.disconnects == 0);
    valkeyClusterAsyncFree(acc);

    /* A pending registration receives one terminal notification on teardown. */
    acc = valkeyClusterAsyncConnectWithOptions(&options);
    assert(acc && !acc->err);
    assert(valkeyClusterAsyncCommand(acc, onSubscription, &replacement, "SSUBSCRIBE %s", channelA) == VALKEY_OK);
    valkeyClusterAsyncFree(acc);
    uv_run(&loop, UV_RUN_DEFAULT);
    assert(replacement.acks == 1 && replacement.disconnects == 1);
    assert(uv_loop_close(&loop) == 0);
}

static void onRedirectedSubscribe(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    valkeyReply *reply = r;
    if (reply && !strcmp(reply->element[0]->str, "ssubscribe")) {
        reply = valkeyClusterCommand(&acc->cc, "SPUBLISH %s payload", channelA);
        CHECK_REPLY_INT((&acc->cc), reply, 1);
        freeReplyObject(reply);
    }
    onSubscription(acc, r, privdata);
}

static void testMoved(const char *seed) {
    uv_loop_t loop;
    assert(uv_loop_init(&loop) == 0);
    valkeyClusterOptions options = {0};
    options.initial_nodes = seed;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    valkeyClusterOptionsUseLibuv(&options, &loop);
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    assert(acc && !acc->err);

    /* Simulate a stale client slot map without changing the cluster. */
    unsigned int slot = valkeyClusterGetSlotByKey(channelA);
    valkeyClusterNode *owner = acc->cc.table[slot];
    valkeyClusterNode *wrong = valkeyClusterGetNodeByKey(&acc->cc, otherChannel);
    assert(owner && wrong && owner != wrong);
    acc->cc.table[slot] = wrong;

    subscription sub = {channelA, VALKEY_REPLY_ARRAY, 0, 0, 0, 0};
    assert(valkeyClusterAsyncCommand(acc, onRedirectedSubscribe, &sub, "SSUBSCRIBE %s", channelA) == VALKEY_OK);
    uv_run(&loop, UV_RUN_DEFAULT);
    assert(sub.acks == 1 && sub.messages == 1 && sub.unsubs == 1 && sub.disconnects == 0);
    valkeyClusterAsyncFree(acc);
    assert(uv_loop_close(&loop) == 0);
}

int main(int argc, char **argv) {
    const char *seed = argc > 1 ? argv[1] : "127.0.0.1:7000";
    valkeyClusterContext *cc = valkeyClusterConnect(seed);
    assert(cc && !cc->err);
    load_valkey_version(cc);
    valkeyClusterFree(cc);
    if (valkey_version_less_than(7, 0))
        return 77;
    run(seed, 2);
    run(seed, 3);
    testMoved(seed);
    return 0;
}
