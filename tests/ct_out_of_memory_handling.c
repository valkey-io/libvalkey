/* Testcases that simulates allocation failures during libvalkeycluster API calls
 * which verifies the handling of out of memory scenarios (OOM).
 *
 * These testcases overrides the default allocators by injecting own functions
 * which can be configured to fail after a given number of successful allocations.
 * A testcase can use a prepare function like `prepare_allocation_test()` to
 * set the number of successful allocations that follows. The allocator will then
 * count the number of calls before it start to return OOM failures, like
 * malloc() returning NULL.
 *
 * Tests will call a libvalkeycluster API-function while iterating on a number,
 * the number of successful allocations during the call before it hits an OOM.
 * The result and the error code is then checked to show "Out of memory".
 * As a last step the correct number of allocations is prepared to get a
 * successful API-function call.
 *
 * Tip:
 * When this testcase fails after code changes in the library, run the testcase
 * in `gdb` to find which API call that failed, and in which iteration.
 * - Go to the correct stack frame to find which API that triggered a failure.
 * - Use the gdb command `print i` to find which iteration.
 * - Investigate if a failure or a success is expected after the code change.
 * - Set correct `i` in for-loop and the `prepare_allocation_test()` for the test.
 *   Correct `i` can be hard to know, finding the correct number might require trial
 *   and error of running with increased/decreased `i` until the edge is found.
 */
#include "adapters/libevent.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_NODE "127.0.0.1:7000"

int successfulAllocations = 0;
bool assertWhenAllocFail = false; // Enable for troubleshooting

// A configurable OOM failing malloc()
static void *vk_malloc_fail(size_t size) {
    if (successfulAllocations > 0) {
        --successfulAllocations;
        return malloc(size);
    }
    assert(assertWhenAllocFail == false);
    return NULL;
}

// A  configurable OOM failing calloc()
static void *vk_calloc_fail(size_t nmemb, size_t size) {
    if (successfulAllocations > 0) {
        --successfulAllocations;
        return calloc(nmemb, size);
    }
    assert(assertWhenAllocFail == false);
    return NULL;
}

// A  configurable OOM failing realloc()
static void *vk_realloc_fail(void *ptr, size_t size) {
    if (successfulAllocations > 0) {
        --successfulAllocations;
        return realloc(ptr, size);
    }
    assert(assertWhenAllocFail == false);
    return NULL;
}

/* Prepare the test fixture.
 * Configures the allocator functions with the number of allocations
 * that will succeed before simulating an out of memory scenario.
 * Additionally it resets errors in the cluster context. */
void prepare_allocation_test(valkeyClusterContext *cc,
                             int _successfulAllocations) {
    successfulAllocations = _successfulAllocations;
    cc->err = 0;
    memset(cc->errstr, '\0', strlen(cc->errstr));
}

void prepare_allocation_test_async(valkeyClusterAsyncContext *acc,
                                   int _successfulAllocations) {
    successfulAllocations = _successfulAllocations;
    acc->err = 0;
    memset(acc->errstr, '\0', strlen(acc->errstr));
}

/* Helper */
valkeyClusterNode *getNodeByPort(valkeyClusterContext *cc, int port) {
    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, cc);
    valkeyClusterNode *node;
    while ((node = valkeyClusterNodeNext(&ni)) != NULL) {
        if (node->port == port)
            return node;
    }
    assert(0);
    return NULL;
}

/* Test of allocation handling in the blocking API */
void test_alloc_failure_handling(void) {
    int result;
    valkeyAllocFuncs ha = {
        .mallocFn = vk_malloc_fail,
        .callocFn = vk_calloc_fail,
        .reallocFn = vk_realloc_fail,
        .strdupFn = strdup,
        .freeFn = free,
    };
    // Override allocators
    valkeySetAllocators(&ha);

    // Context init
    valkeyClusterContext *cc;
    {
        successfulAllocations = 0;
        cc = valkeyClusterContextInit();
        assert(cc == NULL);

        successfulAllocations = 1;
        cc = valkeyClusterContextInit();
        assert(cc);
    }

    // Add nodes
    {
        for (int i = 0; i < 9; ++i) {
            prepare_allocation_test(cc, i);
            result = valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE);
            assert(result == VALKEY_ERR);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");
        }

        prepare_allocation_test(cc, 9);
        result = valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE);
        assert(result == VALKEY_OK);
    }

    // Set connect timeout
    {
        struct timeval timeout = {0, 500000};

        prepare_allocation_test(cc, 0);
        result = valkeyClusterSetOptionConnectTimeout(cc, timeout);
        assert(result == VALKEY_ERR);
        ASSERT_STR_EQ(cc->errstr, "Out of memory");

        prepare_allocation_test(cc, 1);
        result = valkeyClusterSetOptionConnectTimeout(cc, timeout);
        assert(result == VALKEY_OK);
    }

    // Set request timeout
    {
        struct timeval timeout = {0, 500000};

        prepare_allocation_test(cc, 0);
        result = valkeyClusterSetOptionTimeout(cc, timeout);
        assert(result == VALKEY_ERR);
        ASSERT_STR_EQ(cc->errstr, "Out of memory");

        prepare_allocation_test(cc, 1);
        result = valkeyClusterSetOptionTimeout(cc, timeout);
        assert(result == VALKEY_OK);
    }

    // Connect
    {
        for (int i = 0; i < 128; ++i) {
            prepare_allocation_test(cc, i);
            result = valkeyClusterConnect2(cc);
            assert(result == VALKEY_ERR);
        }

        prepare_allocation_test(cc, 128);
        result = valkeyClusterConnect2(cc);
        assert(result == VALKEY_OK);
    }

    // Command
    {
        valkeyReply *reply;
        const char *cmd = "SET key value";

        for (int i = 0; i < 33; ++i) {
            prepare_allocation_test(cc, i);
            reply = (valkeyReply *)valkeyClusterCommand(cc, cmd);
            assert(reply == NULL);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");
        }

        prepare_allocation_test(cc, 33);
        reply = (valkeyReply *)valkeyClusterCommand(cc, cmd);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
    }

    // Command to node
    {
        valkeyReply *reply;
        const char *cmd = "SET key value";

        valkeyClusterNode *node = valkeyClusterGetNodeByKey(cc, (char *)"key");
        assert(node);

        // OOM failing commands
        for (int i = 0; i < 32; ++i) {
            prepare_allocation_test(cc, i);
            reply = valkeyClusterCommandToNode(cc, node, cmd);
            assert(reply == NULL);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");
        }

        // Successful command
        prepare_allocation_test(cc, 32);
        reply = valkeyClusterCommandToNode(cc, node, cmd);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
    }

    // Append command
    {
        valkeyReply *reply;
        const char *cmd = "SET foo one";

        for (int i = 0; i < 34; ++i) {
            prepare_allocation_test(cc, i);
            result = valkeyClusterAppendCommand(cc, cmd);
            assert(result == VALKEY_ERR);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");

            valkeyClusterReset(cc);
        }

        for (int i = 0; i < 4; ++i) {
            // Appended command lost when receiving error from valkey
            // during a GetReply, needs a new append for each test loop
            prepare_allocation_test(cc, 34);
            result = valkeyClusterAppendCommand(cc, cmd);
            assert(result == VALKEY_OK);

            prepare_allocation_test(cc, i);
            result = valkeyClusterGetReply(cc, (void *)&reply);
            assert(result == VALKEY_ERR);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");

            valkeyClusterReset(cc);
        }

        prepare_allocation_test(cc, 34);
        result = valkeyClusterAppendCommand(cc, cmd);
        assert(result == VALKEY_OK);

        prepare_allocation_test(cc, 4);
        result = valkeyClusterGetReply(cc, (void *)&reply);
        assert(result == VALKEY_OK);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
    }

    // Append command to node
    {
        valkeyReply *reply;
        const char *cmd = "SET foo one";

        valkeyClusterNode *node = valkeyClusterGetNodeByKey(cc, (char *)"foo");
        assert(node);

        // OOM failing appends
        for (int i = 0; i < 35; ++i) {
            prepare_allocation_test(cc, i);
            result = valkeyClusterAppendCommandToNode(cc, node, cmd);
            assert(result == VALKEY_ERR);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");

            valkeyClusterReset(cc);
        }

        // OOM failing GetResults
        for (int i = 0; i < 4; ++i) {
            // First a successful append
            prepare_allocation_test(cc, 35);
            result = valkeyClusterAppendCommandToNode(cc, node, cmd);
            assert(result == VALKEY_OK);

            prepare_allocation_test(cc, i);
            result = valkeyClusterGetReply(cc, (void *)&reply);
            assert(result == VALKEY_ERR);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");

            valkeyClusterReset(cc);
        }

        // Successful append and GetReply
        prepare_allocation_test(cc, 35);
        result = valkeyClusterAppendCommandToNode(cc, node, cmd);
        assert(result == VALKEY_OK);

        prepare_allocation_test(cc, 4);
        result = valkeyClusterGetReply(cc, (void *)&reply);
        assert(result == VALKEY_OK);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
    }

    // Redirects
    {
        /* Skip OOM testing during the prepare steps by allowing a high number of
         * allocations. A specific number of allowed allocations will be used later
         * in the testcase when we run commands that results in redirects. */
        prepare_allocation_test(cc, 1000);

        /* Get the source information for the migration. */
        unsigned int slot = valkeyClusterGetSlotByKey((char *)"foo");
        valkeyClusterNode *srcNode = valkeyClusterGetNodeByKey(cc, (char *)"foo");
        int srcPort = srcNode->port;

        /* Get a destination node to migrate the slot to. */
        valkeyClusterNode *dstNode;
        valkeyClusterNodeIterator ni;
        valkeyClusterInitNodeIterator(&ni, cc);
        while ((dstNode = valkeyClusterNodeNext(&ni)) != NULL) {
            if (dstNode != srcNode)
                break;
        }
        assert(dstNode && dstNode != srcNode);
        int dstPort = dstNode->port;

        valkeyReply *reply, *replySrcId, *replyDstId;

        /* Get node id's */
        replySrcId = valkeyClusterCommandToNode(cc, srcNode, "CLUSTER MYID");
        CHECK_REPLY_TYPE(replySrcId, VALKEY_REPLY_STRING);

        replyDstId = valkeyClusterCommandToNode(cc, dstNode, "CLUSTER MYID");
        CHECK_REPLY_TYPE(replyDstId, VALKEY_REPLY_STRING);

        /* Migrate slot */
        reply = valkeyClusterCommandToNode(cc, srcNode,
                                           "CLUSTER SETSLOT %d MIGRATING %s",
                                           slot, replyDstId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
        reply = valkeyClusterCommandToNode(cc, dstNode,
                                           "CLUSTER SETSLOT %d IMPORTING %s",
                                           slot, replySrcId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
        reply = valkeyClusterCommandToNode(
            cc, srcNode, "MIGRATE 127.0.0.1 %d foo 0 5000", dstPort);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);

        /* Test ASK reply handling with OOM */
        for (int i = 0; i < 47; ++i) {
            prepare_allocation_test(cc, i);
            reply = valkeyClusterCommand(cc, "GET foo");
            assert(reply == NULL);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");
        }

        /* Test ASK reply handling without OOM */
        prepare_allocation_test(cc, 47);
        reply = valkeyClusterCommand(cc, "GET foo");
        CHECK_REPLY_STR(cc, reply, "one");
        freeReplyObject(reply);

        /* Finalize the migration. Skip OOM testing during these steps by
         * allowing a high number of allocations. */
        prepare_allocation_test(cc, 1000);
        /* Fetch the nodes again, in case the slotmap has been reloaded. */
        srcNode = valkeyClusterGetNodeByKey(cc, (char *)"foo");
        dstNode = getNodeByPort(cc, dstPort);
        reply = valkeyClusterCommandToNode(
            cc, srcNode, "CLUSTER SETSLOT %d NODE %s", slot, replyDstId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
        reply = valkeyClusterCommandToNode(
            cc, dstNode, "CLUSTER SETSLOT %d NODE %s", slot, replyDstId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);

        /* Test MOVED reply handling with OOM */
        for (int i = 0; i < 31; ++i) {
            prepare_allocation_test(cc, i);
            reply = valkeyClusterCommand(cc, "GET foo");
            assert(reply == NULL);
            ASSERT_STR_EQ(cc->errstr, "Out of memory");
        }

        /* Test MOVED reply handling without OOM */
        prepare_allocation_test(cc, 31);
        reply = valkeyClusterCommand(cc, "GET foo");
        CHECK_REPLY_STR(cc, reply, "one");
        freeReplyObject(reply);

        /* MOVED triggers a slotmap update which currently replaces all cluster_node
         * objects. We can get the new objects by searching for its server ports.
         * This enables us to migrate the slot back to the original node. */
        srcNode = getNodeByPort(cc, srcPort);
        dstNode = getNodeByPort(cc, dstPort);

        /* Migrate back slot, required by the next testcase. Skip OOM testing
         * during these final steps by allowing a high number of allocations. */
        prepare_allocation_test(cc, 1000);
        reply = valkeyClusterCommandToNode(cc, dstNode,
                                           "CLUSTER SETSLOT %d MIGRATING %s",
                                           slot, replySrcId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
        reply = valkeyClusterCommandToNode(cc, srcNode,
                                           "CLUSTER SETSLOT %d IMPORTING %s",
                                           slot, replyDstId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
        reply = valkeyClusterCommandToNode(
            cc, dstNode, "MIGRATE 127.0.0.1 %d foo 0 5000", srcPort);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
        reply = valkeyClusterCommandToNode(
            cc, dstNode, "CLUSTER SETSLOT %d NODE %s", slot, replySrcId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);
        reply = valkeyClusterCommandToNode(
            cc, srcNode, "CLUSTER SETSLOT %d NODE %s", slot, replySrcId->str);
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);

        freeReplyObject(replySrcId);
        freeReplyObject(replyDstId);
    }

    valkeyClusterFree(cc);
    valkeyResetAllocators();
}

//------------------------------------------------------------------------------
// Async API
//------------------------------------------------------------------------------

typedef struct ExpectedResult {
    int type;
    const char *str;
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

// Test of allocation handling in async context
void test_alloc_failure_handling_async(void) {
    int result;
    valkeyAllocFuncs ha = {
        .mallocFn = vk_malloc_fail,
        .callocFn = vk_calloc_fail,
        .reallocFn = vk_realloc_fail,
        .strdupFn = strdup,
        .freeFn = free,
    };
    // Override allocators
    valkeySetAllocators(&ha);

    // Context init
    valkeyClusterAsyncContext *acc;
    {
        for (int i = 0; i < 2; ++i) {
            successfulAllocations = 0;
            acc = valkeyClusterAsyncContextInit();
            assert(acc == NULL);
        }
        successfulAllocations = 2;
        acc = valkeyClusterAsyncContextInit();
        assert(acc);
    }

    // Set callbacks
    {
        prepare_allocation_test_async(acc, 0);
        result = valkeyClusterAsyncSetConnectCallback(acc, callbackExpectOk);
        assert(result == VALKEY_OK);
        result = valkeyClusterAsyncSetDisconnectCallback(acc, callbackExpectOk);
        assert(result == VALKEY_OK);
    }

    // Add nodes
    {
        for (int i = 0; i < 9; ++i) {
            prepare_allocation_test(acc->cc, i);
            result = valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE);
            assert(result == VALKEY_ERR);
            ASSERT_STR_EQ(acc->cc->errstr, "Out of memory");
        }

        prepare_allocation_test(acc->cc, 9);
        result = valkeyClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE);
        assert(result == VALKEY_OK);
    }

    // Connect
    {
        for (int i = 0; i < 126; ++i) {
            prepare_allocation_test(acc->cc, i);
            result = valkeyClusterConnect2(acc->cc);
            assert(result == VALKEY_ERR);
        }

        prepare_allocation_test(acc->cc, 126);
        result = valkeyClusterConnect2(acc->cc);
        assert(result == VALKEY_OK);
    }

    struct event_base *base = event_base_new();
    assert(base);

    successfulAllocations = 0;
    result = valkeyClusterLibeventAttach(acc, base);
    assert(result == VALKEY_OK);

    // Async command 1
    ExpectedResult r1 = {.type = VALKEY_REPLY_STATUS, .str = "OK"};
    {
        const char *cmd1 = "SET foo one";

        for (int i = 0; i < 35; ++i) {
            prepare_allocation_test_async(acc, i);
            result = valkeyClusterAsyncCommand(acc, commandCallback, &r1, cmd1);
            assert(result == VALKEY_ERR);
            if (i != 33) {
                ASSERT_STR_EQ(acc->errstr, "Out of memory");
            } else {
                ASSERT_STR_EQ(acc->errstr, "Failed to attach event adapter");
            }
        }

        prepare_allocation_test_async(acc, 35);
        result = valkeyClusterAsyncCommand(acc, commandCallback, &r1, cmd1);
        ASSERT_MSG(result == VALKEY_OK, acc->errstr);
    }

    // Async command 2
    ExpectedResult r2 = {
        .type = VALKEY_REPLY_STRING, .str = "one", .disconnect = true};
    {
        const char *cmd2 = "GET foo";

        for (int i = 0; i < 12; ++i) {
            prepare_allocation_test_async(acc, i);
            result = valkeyClusterAsyncCommand(acc, commandCallback, &r2, cmd2);
            assert(result == VALKEY_ERR);
            ASSERT_STR_EQ(acc->errstr, "Out of memory");
        }

        /* Skip iteration 12, errstr not set by libvalkey when valkeyFormatSdsCommandArgv() fails. */

        prepare_allocation_test_async(acc, 13);
        result = valkeyClusterAsyncCommand(acc, commandCallback, &r2, cmd2);
        ASSERT_MSG(result == VALKEY_OK, acc->errstr);
    }

    prepare_allocation_test_async(acc, 7);
    event_base_dispatch(base);
    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

int main(void) {

    test_alloc_failure_handling();
    test_alloc_failure_handling_async();

    return 0;
}
