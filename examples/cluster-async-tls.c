#include <valkey/cluster.h>
#include <valkey/cluster_tls.h>

#include <valkey/adapters/libevent.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define CLUSTER_NODE_TLS "127.0.0.1:7300"

void getCallback(valkeyClusterAsyncContext *cc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    if (reply == NULL) {
        if (cc->err) {
            printf("errstr: %s\n", cc->errstr);
        }
        return;
    }
    printf("privdata: %s reply: %s\n", (char *)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
    valkeyClusterAsyncDisconnect(cc);
}

void setCallback(valkeyClusterAsyncContext *cc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    if (reply == NULL) {
        if (cc->err) {
            printf("errstr: %s\n", cc->errstr);
        }
        return;
    }
    printf("privdata: %s reply: %s\n", (char *)privdata, reply->str);
}

void connectCallback(const valkeyAsyncContext *ac, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", ac->errstr);
        return;
    }

    printf("Connected to %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", ac->errstr);
        return;
    }
    printf("Disconnected from %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

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

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE_TLS;
    options.onConnect = connectCallback;
    options.onDisconnect = disconnectCallback;
    options.options = VALKEY_OPT_USE_CLUSTER_SLOTS;
    VALKEY_CLUSTER_OPTIONS_SET_SSL(&options, tls);

    struct event_base *base = event_base_new();
    VALKEY_CLUSTER_OPTIONS_SET_ADAPTER_LIBEVENT(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    if (acc == NULL || acc->err != 0) {
        printf("Error: %s\n", acc ? acc->errstr : "OOM");
        exit(-1);
    }

    int status = valkeyClusterAsyncCommand(acc, setCallback, (char *)"THE_ID",
                                           "SET %s %s", "key", "value");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", acc->err, acc->errstr);
    }

    status = valkeyClusterAsyncCommand(acc, getCallback, (char *)"THE_ID",
                                       "GET %s", "key");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", acc->err, acc->errstr);
    }

    printf("Dispatch..\n");
    event_base_dispatch(base);

    printf("Done..\n");
    valkeyClusterAsyncFree(acc);
    valkeyFreeTLSContext(tls);
    event_base_free(base);
    return 0;
}
