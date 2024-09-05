#include "adapters/libuv.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>

#define CLUSTER_NODE "127.0.0.1:7000"

void setCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    UNUSED(privdata);
    valkeyReply *reply = (valkeyReply *)r;
    ASSERT_MSG(reply != NULL, acc->errstr);
}

void getCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    UNUSED(privdata);
    valkeyReply *reply = (valkeyReply *)r;
    ASSERT_MSG(reply != NULL, acc->errstr);

    /* Disconnect after receiving the first reply to GET */
    valkeyClusterAsyncDisconnect(acc);
}

void connectCallback(const valkeyAsyncContext *ac, int status) {
    ASSERT_MSG(status == VALKEY_OK, ac->errstr);
    printf("Connected to %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    ASSERT_MSG(status == VALKEY_OK, ac->errstr);
    printf("Disconnected from %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

int main(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    valkeyClusterAsyncContext *acc =
        valkeyClusterAsyncConnect(CLUSTER_NODE, VALKEYCLUSTER_FLAG_NULL);
    assert(acc);
    ASSERT_MSG(acc->err == 0, acc->errstr);

    int status;
    uv_loop_t *loop = uv_default_loop();
    status = valkeyClusterLibuvAttach(acc, loop);
    assert(status == VALKEY_OK);

    valkeyClusterAsyncSetConnectCallback(acc, connectCallback);
    valkeyClusterAsyncSetDisconnectCallback(acc, disconnectCallback);

    status = valkeyClusterAsyncCommand(acc, setCallback, (char *)"ID",
                                       "SET key value");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    status =
        valkeyClusterAsyncCommand(acc, getCallback, (char *)"ID", "GET key");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    uv_run(loop, UV_RUN_DEFAULT);

    valkeyClusterAsyncFree(acc);
    uv_loop_delete(loop);
    return 0;
}
