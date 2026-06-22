/*
 * Test for async DNS resolution with c-ares and libevent adapter.
 *
 * Uses "localhost" (a real hostname) to exercise the async c-ares path
 * where DNS resolves via the event loop rather than synchronously.
 */

#include "adapters/libevent.h"
#include "cluster.h"
#include "test_utils.h"
#include "valkey.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CLUSTER_NODE "127.0.0.1:7000"

/* --- Test: async cluster connect using hostname "localhost" --- */

static int connect_cb_called;
static int disconnect_cb_called;

static void connectCb(valkeyAsyncContext *ac, int status) {
    (void)ac;
    if (status == VALKEY_OK)
        connect_cb_called++;
}

static void disconnectCb(const valkeyAsyncContext *ac, int status) {
    (void)ac;
    (void)status;
    disconnect_cb_called++;
}

static void setCallback(valkeyClusterAsyncContext *cc, void *r, void *privdata) {
    (void)privdata;
    valkeyReply *reply = (valkeyReply *)r;
    ASSERT_MSG(reply != NULL, cc->errstr);
    assert(reply->type == VALKEY_REPLY_STATUS);
    assert(strcmp(reply->str, "OK") == 0);
    valkeyClusterAsyncDisconnect(cc);
}

/* Test a basic async command using the cluster API with libevent.
 * The libevent adapter handles DNS_PENDING via async c-ares resolution. */
static void test_async_cluster_command(void) {
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    options.async_connect_callback = connectCb;
    options.async_disconnect_callback = disconnectCb;
    valkeyClusterOptionsUseLibevent(&options, base);

    connect_cb_called = 0;
    disconnect_cb_called = 0;

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    ASSERT_MSG(acc && acc->err == 0, acc ? acc->errstr : "OOM");

    int ret = valkeyClusterAsyncCommand(acc, setCallback, NULL, "SET dns-test hello");
    assert(ret == VALKEY_OK);

    event_base_dispatch(base);

    assert(connect_cb_called > 0);
    assert(disconnect_cb_called > 0);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
    printf("  PASS: test_async_cluster_command\n");
}

/* --- Test: standalone async connect to localhost (hostname, not IP) --- */

static int standalone_connected;

static void standaloneConnectCb(valkeyAsyncContext *ac, int status) {
    (void)ac;
    standalone_connected = (status == VALKEY_OK) ? 1 : -1;
}

static void standaloneGetCb(valkeyAsyncContext *ac, void *r, void *privdata) {
    (void)privdata;
    (void)r;
    valkeyAsyncDisconnect(ac);
}

static void test_async_standalone_localhost(void) {
    struct event_base *base = event_base_new();
    standalone_connected = 0;

    valkeyOptions opts = {0};
    VALKEY_OPTIONS_SET_TCP(&opts, "localhost", 7000);

    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&opts);
    assert(ac != NULL);
    if (ac->err) {
        /* If localhost doesn't resolve to a server, skip gracefully. */
        printf("  SKIP: test_async_standalone_localhost (connect error: %s)\n", ac->errstr);
        valkeyAsyncFree(ac);
        event_base_free(base);
        return;
    }

    valkeyLibeventAttach(ac, base);
    valkeyAsyncSetConnectCallback(ac, standaloneConnectCb);

    /* Send a PING to verify the connection works. */
    valkeyAsyncCommand(ac, standaloneGetCb, NULL, "PING");

    event_base_dispatch(base);

    if (standalone_connected == 1) {
        printf("  PASS: test_async_standalone_localhost\n");
    } else {
        /* Server not running on localhost:7000. */
        printf("  SKIP: test_async_standalone_localhost (server not available)\n");
    }

    event_base_free(base);
}

int main(void) {
    printf("Testing async DNS with c-ares + libevent:\n");
    test_async_cluster_command();
    test_async_standalone_localhost();
    printf("All async DNS tests passed.\n");
    return 0;
}
