#include "adapters/glib.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>

#define CLUSTER_NODE "127.0.0.1:7000"

static GMainLoop *mainloop;

void setCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    UNUSED(privdata);
    valkeyReply *reply = (valkeyReply *)r;
    ASSERT_MSG(reply != NULL, valkeyClusterAsyncGetErrorString(acc));
}

void getCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    UNUSED(privdata);
    valkeyReply *reply = (valkeyReply *)r;
    ASSERT_MSG(reply != NULL, valkeyClusterAsyncGetErrorString(acc));

    /* Disconnect after receiving the first reply to GET */
    valkeyClusterAsyncDisconnect(acc);
    g_main_loop_quit(mainloop);
}

void connectCallback(valkeyAsyncContext *ac, int status) {
    ASSERT_MSG(status == VALKEY_OK, valkeyAsyncGetErrorString(ac));
    printf("Connected to %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    ASSERT_MSG(status == VALKEY_OK, valkeyAsyncGetErrorString(ac));
    printf("Disconnected from %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

int main(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    GMainContext *context = NULL;
    mainloop = g_main_loop_new(context, FALSE);

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    valkeyClusterOptionsUseGlib(&options, context);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    assert(acc);
    ASSERT_MSG(valkeyClusterAsyncGetError(acc) == 0, valkeyClusterAsyncGetErrorString(acc));

    int status;
    status = valkeyClusterAsyncCommand(acc, setCallback, (char *)"id", "SET key value");
    ASSERT_MSG(status == VALKEY_OK, valkeyClusterAsyncGetErrorString(acc));

    status = valkeyClusterAsyncCommand(acc, getCallback, (char *)"id", "GET key");
    ASSERT_MSG(status == VALKEY_OK, valkeyClusterAsyncGetErrorString(acc));

    g_main_loop_run(mainloop);

    valkeyClusterAsyncFree(acc);
    return 0;
}
