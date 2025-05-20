#include <valkey/cluster.h>
#include <valkey/tls.h>

#include <valkey/adapters/libevent.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define CLUSTER_NODE_TLS "127.0.0.1:7300"

void getCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    if (reply == NULL) {
        if (valkeyClusterAsyncGetError(acc)) {
            printf("errstr: %s\n", valkeyClusterAsyncGetErrorString(acc));
        }
        return;
    }
    printf("privdata: %s reply: %s\n", (char *)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
    valkeyClusterAsyncDisconnect(acc);
}

void setCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    if (reply == NULL) {
        if (valkeyClusterAsyncGetError(acc)) {
            printf("errstr: %s\n", valkeyClusterAsyncGetErrorString(acc));
        }
        return;
    }
    printf("privdata: %s reply: %s\n", (char *)privdata, reply->str);
}

void connectCallback(valkeyAsyncContext *ac, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", valkeyAsyncGetErrorString(ac));
        return;
    }

    printf("Connected to %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", valkeyAsyncGetErrorString(ac));
        return;
    }
    printf("Disconnected from %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

int main(void) {
    valkeyTLSContext *tls;
    valkeyTLSContextError tls_error;

    valkeyInitOpenSSL();
    tls = valkeyCreateTLSContext("ca.crt", NULL, "client.crt", "client.key",
                                 NULL, &tls_error);
    if (!tls) {
        printf("TLS Context error: %s\n", valkeyTLSContextGetError(tls_error));
        exit(1);
    }

    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE_TLS;
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    options.tls = tls;
    options.tls_init_fn = &valkeyInitiateTLSWithContext;
    valkeyClusterOptionsUseLibevent(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    if (acc == NULL || valkeyClusterAsyncGetError(acc) != 0) {
        printf("Error: %s\n", acc ? valkeyClusterAsyncGetErrorString(acc) : "OOM");
        exit(-1);
    }

    int status;
    status = valkeyClusterAsyncCommand(acc, setCallback, (char *)"THE_ID",
                                       "SET %s %s", "key", "value");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", valkeyClusterAsyncGetError(acc), valkeyClusterAsyncGetErrorString(acc));
    }

    status = valkeyClusterAsyncCommand(acc, getCallback, (char *)"THE_ID",
                                       "GET %s", "key");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", valkeyClusterAsyncGetError(acc), valkeyClusterAsyncGetErrorString(acc));
    }

    printf("Dispatch..\n");
    event_base_dispatch(base);

    printf("Done..\n");
    valkeyClusterAsyncFree(acc);
    valkeyFreeTLSContext(tls);
    event_base_free(base);
    return 0;
}
