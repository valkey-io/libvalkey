#include "adapters/libevent.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_NODE "127.0.0.1:7000"
#define CLUSTER_NODE_WITH_PASSWORD "127.0.0.1:7100"
#define CLUSTER_USERNAME "default"
#define CLUSTER_PASSWORD "secretword"

int connect_success_counter;
int connect_failure_counter;
void connect_callback(const valkeyContext *c, int status) {
    (void)c;
    if (status == VALKEY_OK)
        connect_success_counter++;
    else
        connect_failure_counter++;
}
void reset_counters(void) {
    connect_success_counter = connect_failure_counter = 0;
}

// Connecting to a password protected cluster and
// providing a correct password.
void test_password_ok(void) {
    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionPassword(cc, CLUSTER_PASSWORD);
    valkeyClusterSetConnectCallback(cc, connect_callback);

    int status;
    status = valkeyClusterConnect2(cc);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    assert(connect_success_counter == 1); // for CLUSTER NODES
    load_valkey_version(cc);
    assert(connect_success_counter == 2); // for checking valkey version

    // Test connection
    valkeyReply *reply;
    reply = valkeyClusterCommand(cc, "SET key1 Hello");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);
    valkeyClusterFree(cc);

    // Check counters incremented by connect callback
    assert(connect_success_counter == 3); // for SET (to a different node)
    assert(connect_failure_counter == 0);
    reset_counters();
}

// Connecting to a password protected cluster and
// providing wrong password.
void test_password_wrong(void) {
    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionPassword(cc, "faultypass");

    int status;
    status = valkeyClusterConnect2(cc);
    assert(status == VALKEY_ERR);

    assert(cc->err == VALKEY_ERR_OTHER);
    if (valkey_version_less_than(6, 0))
        assert(strcmp(cc->errstr, "ERR invalid password") == 0);
    else
        assert(strncmp(cc->errstr, "WRONGPASS", 9) == 0);

    valkeyClusterFree(cc);
}

// Connecting to a password protected cluster and
// not providing any password.
void test_password_missing(void) {
    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE_WITH_PASSWORD);

    // A password is not configured..
    int status;
    status = valkeyClusterConnect2(cc);
    assert(status == VALKEY_ERR);

    assert(cc->err == VALKEY_ERR_OTHER);
    assert(strncmp(cc->errstr, "NOAUTH", 6) == 0);

    valkeyClusterFree(cc);
}

// Connect to a cluster and authenticate using username and password,
// i.e. 'AUTH <username> <password>'
void test_username_ok(void) {
    if (valkey_version_less_than(6, 0))
        return;

    // Connect to the cluster using username and password
    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionUsername(cc, CLUSTER_USERNAME);
    valkeyClusterSetOptionPassword(cc, CLUSTER_PASSWORD);

    int ret = valkeyClusterConnect2(cc);
    ASSERT_MSG(ret == VALKEY_OK, cc->errstr);

    // Test connection
    valkeyReply *reply = valkeyClusterCommand(cc, "SET key1 Hello");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

// Test of disabling the use of username after it was enabled.
void test_username_disabled(void) {
    if (valkey_version_less_than(6, 0))
        return;

    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionUsername(cc, "missing-user");
    valkeyClusterSetOptionPassword(cc, CLUSTER_PASSWORD);

    // Connect using 'AUTH <username> <password>' should fail
    int ret = valkeyClusterConnect2(cc);
    assert(ret == VALKEY_ERR);
    assert(cc->err == VALKEY_ERR_OTHER);
    assert(strncmp(cc->errstr, "WRONGPASS invalid username-password pair",
                   40) == 0);

    // Disable use of username (2 alternatives)
    ret = valkeyClusterSetOptionUsername(cc, NULL);
    ASSERT_MSG(ret == VALKEY_OK, cc->errstr);
    ret = valkeyClusterSetOptionUsername(cc, "");
    ASSERT_MSG(ret == VALKEY_OK, cc->errstr);

    // Connect using 'AUTH <password>' should pass
    ret = valkeyClusterConnect2(cc);
    ASSERT_MSG(ret == VALKEY_OK, cc->errstr);

    // Test connection
    valkeyReply *reply = valkeyClusterCommand(cc, "SET key1 Hello");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

// Connect and handle two clusters simultaneously
void test_multicluster(void) {
    int ret;
    valkeyReply *reply;

    // Connect to first cluster
    valkeyClusterContext *cc1 = valkeyClusterContextInit();
    assert(cc1);
    valkeyClusterSetOptionAddNodes(cc1, CLUSTER_NODE);
    ret = valkeyClusterConnect2(cc1);
    ASSERT_MSG(ret == VALKEY_OK, cc1->errstr);

    // Connect to second cluster
    valkeyClusterContext *cc2 = valkeyClusterContextInit();
    assert(cc2);
    valkeyClusterSetOptionAddNodes(cc2, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionPassword(cc2, CLUSTER_PASSWORD);
    ret = valkeyClusterConnect2(cc2);
    ASSERT_MSG(ret == VALKEY_OK, cc2->errstr);

    // Set keys differently in clusters
    reply = valkeyClusterCommand(cc1, "SET key Hello1");
    CHECK_REPLY_OK(cc1, reply);
    freeReplyObject(reply);

    reply = valkeyClusterCommand(cc2, "SET key Hello2");
    CHECK_REPLY_OK(cc2, reply);
    freeReplyObject(reply);

    // Verify keys in clusters
    reply = valkeyClusterCommand(cc1, "GET key");
    CHECK_REPLY_STR(cc1, reply, "Hello1");
    freeReplyObject(reply);

    reply = valkeyClusterCommand(cc2, "GET key");
    CHECK_REPLY_STR(cc2, reply, "Hello2");
    freeReplyObject(reply);

    // Disconnect from first cluster
    valkeyClusterFree(cc1);

    // Verify that key is still accessible in connected cluster
    reply = valkeyClusterCommand(cc2, "GET key");
    CHECK_REPLY_STR(cc2, reply, "Hello2");
    freeReplyObject(reply);

    valkeyClusterFree(cc2);
}

/* Connect to a non-routable address which results in a connection timeout. */
void test_connect_timeout(void) {
    struct timeval timeout = {0, 200000};

    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);

    /* Configure a non-routable IP address and a timeout */
    valkeyClusterSetOptionAddNodes(cc, "192.168.0.0:7000");
    valkeyClusterSetOptionConnectTimeout(cc, timeout);
    valkeyClusterSetConnectCallback(cc, connect_callback);

    int status = valkeyClusterConnect2(cc);
    assert(status == VALKEY_ERR);
    assert(cc->err == VALKEY_ERR_IO);
    assert(strcmp(cc->errstr, "Connection timed out") == 0);
    assert(connect_success_counter == 0);
    assert(connect_failure_counter == 1);
    reset_counters();

    valkeyClusterFree(cc);
}

/* Connect using a pre-configured command timeout */
void test_command_timeout(void) {
    struct timeval timeout = {0, 10000};

    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE);
    valkeyClusterSetOptionTimeout(cc, timeout);

    int status = valkeyClusterConnect2(cc);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    /* Simulate a command timeout */
    valkeyReply *reply;
    reply = valkeyClusterCommandToNode(cc, node, "DEBUG SLEEP 0.2");
    assert(reply == NULL);
    assert(cc->err == VALKEY_ERR_IO);

    /* Make sure debug sleep is done before leaving testcase */
    for (int i = 0; i < 20; ++i) {
        reply = valkeyClusterCommandToNode(cc, node, "SET key1 Hello");
        if (reply && reply->type == VALKEY_REPLY_STATUS)
            break;
    }
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

/* Connect and configure a command timeout while connected. */
void test_command_timeout_set_while_connected(void) {
    struct timeval timeout = {0, 10000};

    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE);

    int status = valkeyClusterConnect2(cc);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    valkeyReply *reply;
    reply = valkeyClusterCommandToNode(cc, node, "DEBUG SLEEP 0.2");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    /* Set command timeout while connected */
    valkeyClusterSetOptionTimeout(cc, timeout);

    reply = valkeyClusterCommandToNode(cc, node, "DEBUG SLEEP 0.2");
    assert(reply == NULL);
    assert(cc->err == VALKEY_ERR_IO);

    /* Make sure debug sleep is done before leaving testcase */
    for (int i = 0; i < 20; ++i) {
        reply = valkeyClusterCommandToNode(cc, node, "SET key1 Hello");
        if (reply && reply->type == VALKEY_REPLY_STATUS)
            break;
    }
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

//------------------------------------------------------------------------------
// Async API
//------------------------------------------------------------------------------
typedef struct ExpectedResult {
    int type;
    const char *str;
    bool disconnect;
    bool noreply;
    const char *errstr;
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
    if (expect->noreply) {
        assert(reply == NULL);
        assert(strcmp(cc->errstr, expect->errstr) == 0);
    } else {
        assert(reply != NULL);
        assert(reply->type == expect->type);
        if (reply->type == VALKEY_REPLY_ERROR ||
            reply->type == VALKEY_REPLY_STATUS ||
            reply->type == VALKEY_REPLY_STRING ||
            reply->type == VALKEY_REPLY_DOUBLE ||
            reply->type == VALKEY_REPLY_VERB) {
            assert(strcmp(reply->str, expect->str) == 0);
        }
    }
    if (expect->disconnect)
        valkeyClusterAsyncDisconnect(cc);
}

// Connecting to a password protected cluster using
// the async API, providing correct password.
void test_async_password_ok(void) {
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);
    valkeyClusterAsyncSetConnectCallback(acc, callbackExpectOk);
    valkeyClusterAsyncSetDisconnectCallback(acc, callbackExpectOk);
    valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionPassword(acc->cc, CLUSTER_PASSWORD);

    struct event_base *base = event_base_new();
    valkeyClusterLibeventAttach(acc, base);

    int ret;
    ret = valkeyClusterConnect2(acc->cc);
    assert(ret == VALKEY_OK);
    assert(acc->err == 0);
    assert(acc->cc->err == 0);

    // Test connection
    ExpectedResult r = {
        .type = VALKEY_REPLY_STATUS, .str = "OK", .disconnect = true};
    ret = valkeyClusterAsyncCommand(acc, commandCallback, &r, "SET key1 Hello");
    assert(ret == VALKEY_OK);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

/* Connect to a password protected cluster using the wrong password.
   An eventloop is not attached since it is not needed is this case. */
void test_async_password_wrong(void) {
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);
    valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionPassword(acc->cc, "faultypass");

    int ret;
    ret = valkeyClusterConnect2(acc->cc);
    assert(ret == VALKEY_ERR);
    assert(acc->err == VALKEY_OK); // TODO: This must be wrong!
    assert(acc->cc->err == VALKEY_ERR_OTHER);
    if (valkey_version_less_than(6, 0))
        assert(strcmp(acc->cc->errstr, "ERR invalid password") == 0);
    else
        assert(strncmp(acc->cc->errstr, "WRONGPASS", 9) == 0);

    // No connection
    ExpectedResult r;
    ret = valkeyClusterAsyncCommand(acc, commandCallback, &r, "SET key1 Hello");
    assert(ret == VALKEY_ERR);
    assert(acc->err == VALKEY_ERR_OTHER);
    assert(strcmp(acc->errstr, "slotmap not available") == 0);

    valkeyClusterAsyncFree(acc);
}

/* Connect to a password protected cluster without providing a password.
   An eventloop is not attached since it is not needed is this case. */
void test_async_password_missing(void) {
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);
    valkeyClusterAsyncSetConnectCallback(acc, callbackExpectOk);
    valkeyClusterAsyncSetDisconnectCallback(acc, callbackExpectOk);
    valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE_WITH_PASSWORD);
    // Password not configured

    int ret;
    ret = valkeyClusterConnect2(acc->cc);
    assert(ret == VALKEY_ERR);
    assert(acc->err == VALKEY_OK); // TODO: This must be wrong!
    assert(acc->cc->err == VALKEY_ERR_OTHER);
    assert(strncmp(acc->cc->errstr, "NOAUTH", 6) == 0);

    // No connection
    ExpectedResult r;
    ret = valkeyClusterAsyncCommand(acc, commandCallback, &r, "SET key1 Hello");
    assert(ret == VALKEY_ERR);
    assert(acc->err == VALKEY_ERR_OTHER);
    assert(strcmp(acc->errstr, "slotmap not available") == 0);

    valkeyClusterAsyncFree(acc);
}

// Connect to a cluster and authenticate using username and password
void test_async_username_ok(void) {
    if (valkey_version_less_than(6, 0))
        return;

    // Connect to the cluster using username and password
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);
    valkeyClusterAsyncSetConnectCallback(acc, callbackExpectOk);
    valkeyClusterAsyncSetDisconnectCallback(acc, callbackExpectOk);
    valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionUsername(acc->cc, "missing-user");
    valkeyClusterSetOptionPassword(acc->cc, CLUSTER_PASSWORD);

    struct event_base *base = event_base_new();
    valkeyClusterLibeventAttach(acc, base);

    // Connect using wrong username should fail
    int ret = valkeyClusterConnect2(acc->cc);
    assert(ret == VALKEY_ERR);
    assert(acc->cc->err == VALKEY_ERR_OTHER);
    assert(strncmp(acc->cc->errstr, "WRONGPASS invalid username-password pair",
                   40) == 0);

    // Set correct username
    ret = valkeyClusterSetOptionUsername(acc->cc, CLUSTER_USERNAME);
    ASSERT_MSG(ret == VALKEY_OK, acc->cc->errstr);

    // Connect using correct username should pass
    ret = valkeyClusterConnect2(acc->cc);
    assert(ret == VALKEY_OK);
    assert(acc->err == 0);
    assert(acc->cc->err == 0);

    // Test connection
    ExpectedResult r = {
        .type = VALKEY_REPLY_STATUS, .str = "OK", .disconnect = true};
    ret = valkeyClusterAsyncCommand(acc, commandCallback, &r, "SET key1 Hello");
    assert(ret == VALKEY_OK);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

// Connect and handle two clusters simultaneously using the async API
void test_async_multicluster(void) {
    int ret;

    valkeyClusterAsyncContext *acc1 = valkeyClusterAsyncContextInit();
    assert(acc1);
    valkeyClusterAsyncSetConnectCallback(acc1, callbackExpectOk);
    valkeyClusterAsyncSetDisconnectCallback(acc1, callbackExpectOk);
    valkeyClusterSetOptionAddNodes(acc1->cc, CLUSTER_NODE);

    valkeyClusterAsyncContext *acc2 = valkeyClusterAsyncContextInit();
    assert(acc2);
    valkeyClusterAsyncSetConnectCallback(acc2, callbackExpectOk);
    valkeyClusterAsyncSetDisconnectCallback(acc2, callbackExpectOk);
    valkeyClusterSetOptionAddNodes(acc2->cc, CLUSTER_NODE_WITH_PASSWORD);
    valkeyClusterSetOptionPassword(acc2->cc, CLUSTER_PASSWORD);

    struct event_base *base = event_base_new();
    valkeyClusterLibeventAttach(acc1, base);
    valkeyClusterLibeventAttach(acc2, base);

    // Connect to first cluster
    ret = valkeyClusterConnect2(acc1->cc);
    assert(ret == VALKEY_OK);
    assert(acc1->err == 0);
    assert(acc1->cc->err == 0);

    // Connect to second cluster
    ret = valkeyClusterConnect2(acc2->cc);
    assert(ret == VALKEY_OK);
    assert(acc2->err == 0);
    assert(acc2->cc->err == 0);

    // Set keys differently in clusters
    ExpectedResult r1 = {.type = VALKEY_REPLY_STATUS, .str = "OK"};
    ret = valkeyClusterAsyncCommand(acc1, commandCallback, &r1, "SET key A");
    assert(ret == VALKEY_OK);

    ExpectedResult r2 = {.type = VALKEY_REPLY_STATUS, .str = "OK"};
    ret = valkeyClusterAsyncCommand(acc2, commandCallback, &r2, "SET key B");
    assert(ret == VALKEY_OK);

    // Verify key in first cluster
    ExpectedResult r3 = {.type = VALKEY_REPLY_STRING, .str = "A"};
    ret = valkeyClusterAsyncCommand(acc1, commandCallback, &r3, "GET key");
    assert(ret == VALKEY_OK);

    // Verify key in second cluster and disconnect
    ExpectedResult r4 = {
        .type = VALKEY_REPLY_STRING, .str = "B", .disconnect = true};
    ret = valkeyClusterAsyncCommand(acc2, commandCallback, &r4, "GET key");
    assert(ret == VALKEY_OK);

    // Verify that key is still accessible in connected cluster
    ExpectedResult r5 = {
        .type = VALKEY_REPLY_STRING, .str = "A", .disconnect = true};
    ret = valkeyClusterAsyncCommand(acc1, commandCallback, &r5, "GET key");
    assert(ret == VALKEY_OK);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc1);
    valkeyClusterAsyncFree(acc2);
    event_base_free(base);
}

/* Connect to a non-routable address which results in a connection timeout. */
void test_async_connect_timeout(void) {
    struct timeval timeout = {0, 200000};

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);

    /* Configure a non-routable IP address and a timeout */
    valkeyClusterSetOptionAddNodes(acc->cc, "192.168.0.0:7000");
    valkeyClusterSetOptionConnectTimeout(acc->cc, timeout);

    struct event_base *base = event_base_new();
    valkeyClusterLibeventAttach(acc, base);

    int status = valkeyClusterConnect2(acc->cc);
    assert(status == VALKEY_ERR);
    assert(acc->cc->err == VALKEY_ERR_IO);
    assert(strcmp(acc->cc->errstr, "Connection timed out") == 0);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

/* Connect using a pre-configured command timeout */
void test_async_command_timeout(void) {
    struct timeval timeout = {0, 10000};

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);
    valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE);
    valkeyClusterSetOptionTimeout(acc->cc, timeout);

    struct event_base *base = event_base_new();
    valkeyClusterLibeventAttach(acc, base);

    int status = valkeyClusterConnect2(acc->cc);
    assert(status == VALKEY_OK);
    assert(acc->cc->err == 0);

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, acc->cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    /* Simulate a command timeout and expect a timeout error */
    ExpectedResult r = {
        .noreply = true, .errstr = "Timeout", .disconnect = true};
    status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback, &r,
                                             "DEBUG SLEEP 0.2");
    assert(status == VALKEY_OK);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

int main(void) {

    test_password_ok();
    test_password_wrong();
    test_password_missing();
    test_username_ok();
    test_username_disabled();
    test_multicluster();
    test_connect_timeout();
    test_command_timeout();
    test_command_timeout_set_while_connected();

    test_async_password_ok();
    test_async_password_wrong();
    test_async_password_missing();
    test_async_username_ok();
    test_async_multicluster();
    test_async_connect_timeout();
    test_async_command_timeout();

    return 0;
}
