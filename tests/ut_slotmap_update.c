/* Unit tests of internal functions that parses node and slot information
 * during slotmap updates. */

#ifndef __has_feature
#define __has_feature(feature) 0
#endif

/* Disable the 'One Definition Rule' check if running with address sanitizer
 * since we will include a sourcefile but also link to the library. */
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
const char *__asan_default_options(void) {
    return "detect_odr_violation=0";
}
#endif

/* Includes source file to test static functions. */
#include "cluster.c"

#include <stdbool.h>

/* Helper to create a valkeyReply that contains a bulkstring. */
valkeyReply *create_cluster_nodes_reply(const char *bulkstr) {
    valkeyReply *reply;

    /* Create a RESP Bulk String. */
    char cmd[1024];
    int len = sprintf(cmd, "$%zu\r\n%s\r\n", strlen(bulkstr), bulkstr);

    /* Create a valkeyReply. */
    valkeyReader *reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, cmd, len);
    assert(valkeyReaderGetReply(reader, (void **)&reply) == VALKEY_OK);
    valkeyReaderFree(reader);
    return reply;
}

/* Parse a cluster nodes reply from a basic deployment. */
void test_parse_cluster_nodes(bool parse_replicas) {
    valkeyClusterOptions options = {0};
    if (parse_replicas)
        options.options |= VALKEY_OPT_USE_REPLICAS;

    valkeyClusterContext *cc = valkeyClusterContextInit(&options);
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;

    valkeyReply *reply = create_cluster_nodes_reply(
        "07c37dfeb235213a872192d90877d0cd55635b91 127.0.0.1:30004@31004,hostname4 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238317239 4 connected\n"
        "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 127.0.0.1:30002@31002,hostname2 master - 0 1426238316232 2 connected 5461-10922\n"
        "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 127.0.0.1:30003@31003,hostname3 master - 0 1426238318243 3 connected 10923-16383\n"
        "6ec23923021cf3ffec47632106199cb7f496ce01 127.0.0.1:30005@31005,hostname5 slave 67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 0 1426238316232 5 connected\n"
        "824fe116063bc5fcf9f4ffd895bc17aee7731ac3 127.0.0.1:30006@31006,hostname6 slave 292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 0 1426238317741 6 connected\n"
        "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 127.0.0.1:30001@31001,hostname1 myself,master - 0 0 1 connected 0-5460\n");
    dict *nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 3); /* 3 masters */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30001);
    assert(node->role == VALKEY_ROLE_MASTER);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 5460);
    if (parse_replicas) {
        assert(listLength(node->slaves) == 1);
        node = listNodeValue(listFirst(node->slaves));
        assert(strcmp(node->name, "07c37dfeb235213a872192d90877d0cd55635b91") == 0);
        assert(node->role == VALKEY_ROLE_SLAVE);
    } else {
        assert(node->slaves == NULL);
    }
    /* Verify node 2 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30002") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30002);
    assert(node->role == VALKEY_ROLE_MASTER);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 5461);
    assert(slot->end == 10922);
    if (parse_replicas) {
        assert(listLength(node->slaves) == 1);
        node = listNodeValue(listFirst(node->slaves));
        assert(strcmp(node->name, "6ec23923021cf3ffec47632106199cb7f496ce01") == 0);
        assert(node->role == VALKEY_ROLE_SLAVE);
    } else {
        assert(node->slaves == NULL);
    }
    /* Verify node 3 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30003") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30003);
    assert(node->role == VALKEY_ROLE_MASTER);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 10923);
    assert(slot->end == 16383);
    if (parse_replicas) {
        assert(listLength(node->slaves) == 1);
        node = listNodeValue(listFirst(node->slaves));
        assert(strcmp(node->name, "824fe116063bc5fcf9f4ffd895bc17aee7731ac3") == 0);
        assert(node->role == VALKEY_ROLE_SLAVE);
    } else {
        assert(node->slaves == NULL);
    }

    dictRelease(nodes);
    valkeyClusterFree(cc);
}

void test_parse_cluster_nodes_during_failover(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = valkeyClusterContextInit(&options);
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;

    /* 10.10.10.122 crashed and 10.10.10.126 promoted to master. */
    valkeyReply *reply = create_cluster_nodes_reply(
        "184ada329264e994781412f3986c425a248f386e 10.10.10.126:7000@17000 master - 0 1625255654350 7 connected 5461-10922\n"
        "5cc0f693985913c553c6901e102ea3cb8d6678bd 10.10.10.122:7000@17000 master,fail - 1625255622147 1625255621143 2 disconnected\n"
        "22de56650b3714c1c42fc0d120f80c66c24d8795 10.10.10.123:7000@17000 master - 0 1625255654000 3 connected 10923-16383\n"
        "ad0f5210dda1736a1b5467cd6e797f011a192097 10.10.10.125:7000@17000 slave 4394d8eb03de1f524b56cb385f0eb9052ce65283 0 1625255656366 1 connected\n"
        "8675cd30fdd4efa088634e50fbd5c0675238a35e 10.10.10.124:7000@17000 slave 22de56650b3714c1c42fc0d120f80c66c24d8795 0 1625255655360 3 connected\n"
        "4394d8eb03de1f524b56cb385f0eb9052ce65283 10.10.10.121:7000@17000 myself,master - 0 1625255653000 1 connected 0-5460\n");
    dict *nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 4); /* 4 masters */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "5cc0f693985913c553c6901e102ea3cb8d6678bd") == 0);
    assert(strcmp(node->addr, "10.10.10.122:7000") == 0);
    assert(strcmp(node->host, "10.10.10.122") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 0); /* No slots (fail flag). */
    /* Verify node 2 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "184ada329264e994781412f3986c425a248f386e") == 0);
    assert(strcmp(node->addr, "10.10.10.126:7000") == 0);
    assert(strcmp(node->host, "10.10.10.126") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 5461);
    assert(slot->end == 10922);
    /* Verify node 3 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "22de56650b3714c1c42fc0d120f80c66c24d8795") == 0);
    assert(strcmp(node->addr, "10.10.10.123:7000") == 0);
    assert(strcmp(node->host, "10.10.10.123") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 10923);
    assert(slot->end == 16383);
    /* Verify node 4 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "4394d8eb03de1f524b56cb385f0eb9052ce65283") == 0);
    assert(strcmp(node->addr, "10.10.10.121:7000") == 0);
    assert(strcmp(node->host, "10.10.10.121") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 5460);

    dictRelease(nodes);
    valkeyClusterFree(cc);
}

/* Skip nodes with no address, i.e with address :0 */
void test_parse_cluster_nodes_with_noaddr(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = valkeyClusterContextInit(&options);
    valkeyClusterNode *node;
    dictIterator di;

    valkeyReply *reply = create_cluster_nodes_reply(
        "752d150249c157c7cb312b6b056517bbbecb42d2 :0@0 master,noaddr - 1658754833817 1658754833000 3 disconnected 5461-10922\n"
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0:6379@16379 myself,master - 0 1658755601000 1 connected 0-5460\n"
        "87f785c4a51f58c06e4be55de8c112210a811db9 127.0.0.2:6379@16379 master - 0 1658755602418 3 connected 10923-16383\n");
    dict *nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 2); /* Only 2 masters since ":0" is skipped. */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.0:6379") == 0);
    /* Verify node 2 */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.2:6379") == 0);

    dictRelease(nodes);
    valkeyClusterFree(cc);
}

/* Parse replies with additional importing and migrating information. */
void test_parse_cluster_nodes_with_special_slot_entries(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = valkeyClusterContextInit(&options);
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;
    listIter li;

    /* The reply contains special slot entries with migrating slot and
     * importing slot information that will be ignored. */
    valkeyReply *reply = create_cluster_nodes_reply(
        "4394d8eb03de1f524b56cb385f0eb9052ce65283 10.10.10.121:7000@17000 myself,master - 0 1625255653000 1 connected 0 2-5460 [0->-e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca] [1-<-292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f]\n");
    dict *nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 1); /* 1 master */
    dictInitIterator(&di, nodes);
    /* Verify node. */
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "4394d8eb03de1f524b56cb385f0eb9052ce65283") == 0);
    assert(strcmp(node->addr, "10.10.10.121:7000") == 0);
    assert(strcmp(node->host, "10.10.10.121") == 0);
    assert(node->port == 7000);
    /* Verify slots in node. */
    assert(listLength(node->slots) == 2); /* 2 slot ranges */
    listRewind(node->slots, &li);
    slot = listNodeValue(listNext(&li));
    assert(slot->start == 0);
    assert(slot->end == 0);
    slot = listNodeValue(listNext(&li));
    assert(slot->start == 2);
    assert(slot->end == 5460);

    dictRelease(nodes);
    valkeyClusterFree(cc);
}

/* Parse a cluster nodes reply containing a primary with multiple replicas. */
void test_parse_cluster_nodes_with_multiple_replicas(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = valkeyClusterContextInit(&options);
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;
    listIter li;

    cc->flags |= VALKEY_FLAG_PARSE_REPLICAS;

    valkeyReply *reply = create_cluster_nodes_reply(
        "07c37dfeb235213a872192d90877d0cd55635b91 127.0.0.1:30004@31004,hostname4 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238317239 4 connected\n"
        "6ec23923021cf3ffec47632106199cb7f496ce01 127.0.0.1:30005@31005,hostname5 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238316232 5 connected\n"
        "824fe116063bc5fcf9f4ffd895bc17aee7731ac3 127.0.0.1:30006@31006,hostname6 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238317741 6 connected\n"
        "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 127.0.0.1:30001@31001,hostname1 myself,master - 0 0 1 connected 0-16383\n"
        "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 127.0.0.1:30002@31002,hostname2 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238316232 2 connected\n"
        "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 127.0.0.1:30003@31003,hostname3 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238318243 3 connected\n");
    dict *nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);

    /* Verify master. */
    assert(nodes);
    assert(dictSize(nodes) == 1); /* 1 master */
    dictInitIterator(&di, nodes);
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->name, "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30001);
    assert(node->role == VALKEY_ROLE_MASTER);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 16383);

    /* Verify replicas. */
    assert(listLength(node->slaves) == 5);
    listRewind(node->slaves, &li);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "07c37dfeb235213a872192d90877d0cd55635b91") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30004") == 0);
    assert(node->role == VALKEY_ROLE_SLAVE);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "6ec23923021cf3ffec47632106199cb7f496ce01") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30005") == 0);
    assert(node->role == VALKEY_ROLE_SLAVE);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "824fe116063bc5fcf9f4ffd895bc17aee7731ac3") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30006") == 0);
    assert(node->role == VALKEY_ROLE_SLAVE);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30002") == 0);
    assert(node->role == VALKEY_ROLE_SLAVE);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30003") == 0);
    assert(node->role == VALKEY_ROLE_SLAVE);

    dictRelease(nodes);
    valkeyClusterFree(cc);
}

/* Give error when parsing erroneous data. */
void test_parse_cluster_nodes_with_parse_error(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = valkeyClusterContextInit(&options);
    valkeyReply *reply;
    dict *nodes;

    /* Missing link-state (and slots). */
    reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0:30001@31001 myself,master - 0 1658755601000 1 \n");
    nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);
    assert(nodes == NULL);
    assert(cc->err == VALKEY_ERR_OTHER);
    valkeyClusterClearError(cc);

    /* Missing port. */
    reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0@31001 myself,master - 0 1658755601000 1 connected 0-5460\n");
    nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);
    assert(nodes == NULL);
    assert(cc->err == VALKEY_ERR_OTHER);
    valkeyClusterClearError(cc);

    /* Missing port and cport. */
    reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0 myself,master - 0 1658755601000 1 connected 0-5460\n");
    nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);
    assert(nodes == NULL);
    assert(cc->err == VALKEY_ERR_OTHER);
    valkeyClusterClearError(cc);

    valkeyClusterFree(cc);
}

/* Redis pre-v4.0 returned node addresses without the clusterbus port,
 * i.e. `ip:port` instead of `ip:port@cport` */
void test_parse_cluster_nodes_with_legacy_format(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = valkeyClusterContextInit(&options);
    valkeyClusterNode *node;
    dictIterator di;

    valkeyReply *reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0:6379 myself,master - 0 1658755601000 1 connected 0-5460\n"
        "752d150249c157c7cb312b6b056517bbbecb42d2 :0 master,noaddr - 1658754833817 1658754833000 3 disconnected 5461-10922\n");
    dict *nodes = parse_cluster_nodes(cc, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 1); /* Only 1 master since ":0" is skipped. */
    dictInitIterator(&di, nodes);
    node = dictGetEntryVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.0:6379") == 0);

    dictRelease(nodes);
    valkeyClusterFree(cc);
}

int main(void) {
    test_parse_cluster_nodes(false /* replicas not parsed */);
    test_parse_cluster_nodes(true /* replicas parsed */);
    test_parse_cluster_nodes_during_failover();
    test_parse_cluster_nodes_with_noaddr();
    test_parse_cluster_nodes_with_special_slot_entries();
    test_parse_cluster_nodes_with_multiple_replicas();
    test_parse_cluster_nodes_with_parse_error();
    test_parse_cluster_nodes_with_legacy_format();
    return 0;
}
