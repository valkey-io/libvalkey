#include <valkey/cluster.h>
#include <valkey/cluster_tls.h>
#include <valkey/tls.h>
#include <valkey/valkey.h>

#include <stdio.h>
#include <stdlib.h>

#define CLUSTER_NODE_TLS "127.0.0.1:7301"

int main(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    valkeyTLSContext *tls;
    valkeyTLSContextError tls_error;

    valkeyInitOpenSSL();
    tls = valkeyCreateTLSContext("ca.crt", NULL, "client.crt", "client.key",
                                 NULL, &tls_error);
    if (!tls) {
        printf("TLS Context error: %s\n", valkeyTLSContextGetError(tls_error));
        exit(1);
    }

    struct timeval timeout = {1, 500000}; // 1.5s

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE_TLS;
    options.options = VALKEY_OPT_USE_CLUSTER_SLOTS;
    options.connect_timeout = &timeout;
    valkeyClusterOptionsEnableTLS(&options, tls);

    valkeyClusterContext *cc = valkeyClusterConnectWithOptions(&options);
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
    valkeyFreeTLSContext(tls);
    return 0;
}
