#include <valkey/cluster.h>
#include <valkey/cluster_ssl.h>
#include <valkey/ssl.h>
#include <valkey/valkey.h>

#include <stdio.h>
#include <stdlib.h>

#define CLUSTER_NODE_TLS "127.0.0.1:7301"

int main(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    valkeySSLContext *ssl;
    valkeySSLContextError ssl_error;

    valkeyInitOpenSSL();
    ssl = valkeyCreateSSLContext("ca.crt", NULL, "client.crt", "client.key",
                                 NULL, &ssl_error);
    if (!ssl) {
        printf("SSL Context error: %s\n", valkeySSLContextGetError(ssl_error));
        exit(1);
    }

    struct timeval timeout = {1, 500000}; // 1.5s

    valkeyClusterContext *cc = valkeyClusterContextInit();
    valkeyClusterSetOptionAddNodes(cc, CLUSTER_NODE_TLS);
    valkeyClusterSetOptionConnectTimeout(cc, timeout);
    valkeyClusterSetOptionRouteUseSlots(cc);
    valkeyClusterSetOptionParseSlaves(cc);
    valkeyClusterSetOptionEnableSSL(cc, ssl);
    valkeyClusterConnect2(cc);
    if (!cc) {
        printf("Error: Allocation failure\n");
        exit(-1);
    } else if (cc->err) {
        printf("Error: %s\n", cc->errstr);
        // handle error
        exit(-1);
    }

    valkeyReply *reply = valkeyClusterCommand(cc, "SET %s %s", "key", "value");
    if (!reply) {
        printf("Reply missing: %s\n", cc->errstr);
        exit(-1);
    }
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);

    valkeyReply *reply2 = valkeyClusterCommand(cc, "GET %s", "key");
    if (!reply2) {
        printf("Reply missing: %s\n", cc->errstr);
        exit(-1);
    }
    printf("GET: %s\n", reply2->str);
    freeReplyObject(reply2);

    valkeyClusterFree(cc);
    valkeyFreeSSLContext(ssl);
    return 0;
}
