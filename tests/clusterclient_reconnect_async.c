/*
 * This program connects to a Valkey node and then reads commands from stdin, such
 * as "SET foo bar", one per line and prints the results to stdout.
 *
 * The behaviour is similar to that of clusterclient_async.c, but it sends the
 * next command after receiving a reply from the previous command. It also works
 * for standalone Valkey nodes (without cluster mode), and uses the
 * valkeyClusterAsyncCommandToNode function to send the command to the first node.
 * If it receives any I/O error, the program performs a reconnect.
 */

#include "adapters/libevent.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unfortunately there is no error code for this error to match */
#define VALKEY_ENOCLUSTER "ERR This instance has cluster support disabled"

void sendNextCommand(evutil_socket_t, short, void *);

void connectToValkey(valkeyClusterAsyncContext *acc) {
    /* reset context in case of reconnect */
    valkeyClusterAsyncDisconnect(acc);

    int status = valkeyClusterConnect2(acc->cc);
    if (status == VALKEY_OK) {
        // cluster mode
    } else if (acc->cc->err &&
               strcmp(acc->cc->errstr, VALKEY_ENOCLUSTER) == 0) {
        printf("[no cluster]\n");
        acc->cc->err = 0;
        memset(acc->cc->errstr, '\0', strlen(acc->cc->errstr));
    } else {
        printf("Connect error: %s\n", acc->cc->errstr);
        exit(-1);
    }
}

void replyCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    UNUSED(privdata);
    valkeyReply *reply = (valkeyReply *)r;

    if (reply == NULL) {
        if (acc->err == VALKEY_ERR_IO || acc->err == VALKEY_ERR_EOF) {
            printf("[reconnect]\n");
            connectToValkey(acc);
        } else if (acc->err) {
            printf("error: %s\n", acc->errstr);
        } else {
            printf("unknown error\n");
        }
    } else {
        printf("%s\n", reply->str);
    }

    // schedule reading from stdin and sending next command
    event_base_once(acc->adapter, -1, EV_TIMEOUT, sendNextCommand, acc, NULL);
}

void sendNextCommand(evutil_socket_t fd, short kind, void *arg) {
    UNUSED(fd);
    UNUSED(kind);
    valkeyClusterAsyncContext *acc = arg;

    char command[256];
    if (fgets(command, 256, stdin)) {
        size_t len = strlen(command);
        if (command[len - 1] == '\n') // Chop trailing line break
            command[len - 1] = '\0';

        dictIterator di;
        dictInitIterator(&di, acc->cc->nodes);

        dictEntry *de = dictNext(&di);
        assert(de);
        valkeyClusterNode *node = dictGetEntryVal(de);
        assert(node);

        // coverity[tainted_scalar]
        int status = valkeyClusterAsyncCommandToNode(acc, node, replyCallback,
                                                     NULL, command);
        ASSERT_MSG(status == VALKEY_OK, acc->errstr);
    } else {
        // disconnect if nothing is left to read from stdin
        valkeyClusterAsyncDisconnect(acc);
    }
}

int main(int argc, char **argv) {
    if (argc <= 1) {
        fprintf(stderr, "Usage: %s HOST:PORT\n", argv[0]);
        exit(1);
    }
    const char *initnode = argv[1];

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncContextInit();
    assert(acc);
    valkeyClusterSetOptionAddNodes(acc->cc, initnode);
    valkeyClusterSetOptionRouteUseSlots(acc->cc);

    struct event_base *base = event_base_new();
    int status = valkeyClusterLibeventAttach(acc, base);
    assert(status == VALKEY_OK);

    connectToValkey(acc);
    // schedule reading from stdin and sending next command
    event_base_once(acc->adapter, -1, EV_TIMEOUT, sendNextCommand, acc, NULL);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
    return 0;
}
