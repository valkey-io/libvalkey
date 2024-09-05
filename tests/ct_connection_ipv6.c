#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_NODE_IPV6 "::1:7200"

// Successful connection an IPv6 cluster
void test_successful_ipv6_connection(void) {

    valkeyClusterContext *cc = valkeyClusterContextInit();
    assert(cc);

    int status;
    struct timeval timeout = {0, 500000}; // 0.5s
    status = valkeyClusterSetOptionConnectTimeout(cc, timeout);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE_IPV6);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterSetOptionRouteUseSlots(cc);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    status = valkeyClusterConnect2(cc);
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    valkeyReply *reply;
    reply = (valkeyReply *)valkeyClusterCommand(cc, "SET key_ipv6 value");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    reply = (valkeyReply *)valkeyClusterCommand(cc, "GET key_ipv6");
    CHECK_REPLY_STR(cc, reply, "value");
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

int main(void) {

    test_successful_ipv6_connection();

    return 0;
}
