#include <valkey/cluster.h>

#include <valkey/adapters/libevent.h>

#include <stdio.h>
#include <stdlib.h>

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
    (void)argc;
    (void)argv;
    printf("Connecting...\n");
    valkeyClusterAsyncContext *cc =
        valkeyClusterAsyncConnect("127.0.0.1:7000", VALKEYCLUSTER_FLAG_NULL);
    if (!cc) {
        printf("Error: Allocation failure\n");
        exit(-1);
    } else if (cc->err) {
        printf("Error: %s\n", cc->errstr);
        // handle error
        exit(-1);
    }

    struct event_base *base = event_base_new();
    valkeyClusterLibeventAttach(cc, base);
    valkeyClusterAsyncSetConnectCallback(cc, connectCallback);
    valkeyClusterAsyncSetDisconnectCallback(cc, disconnectCallback);

    int status;
    status = valkeyClusterAsyncCommand(cc, setCallback, (char *)"THE_ID",
                                       "SET %s %s", "key", "value");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", cc->err, cc->errstr);
    }

    status = valkeyClusterAsyncCommand(cc, getCallback, (char *)"THE_ID",
                                       "GET %s", "key");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", cc->err, cc->errstr);
    }

    status = valkeyClusterAsyncCommand(cc, setCallback, (char *)"THE_ID",
                                       "SET %s %s", "key2", "value2");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", cc->err, cc->errstr);
    }

    status = valkeyClusterAsyncCommand(cc, getCallback, (char *)"THE_ID",
                                       "GET %s", "key2");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", cc->err, cc->errstr);
    }

    printf("Dispatch..\n");
    event_base_dispatch(base);

    printf("Done..\n");
    valkeyClusterAsyncFree(cc);
    event_base_free(base);
    return 0;
}
