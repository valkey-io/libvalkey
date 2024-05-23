#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <valkey.h>
#include <async.h>
#include <adapters/libhv.h>

void getCallback(valkeyAsyncContext *c, void *r, void *privdata) {
    valkeyReply *reply = r;
    if (reply == NULL) return;
    printf("argv[%s]: %s\n", (char*)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
    valkeyAsyncDisconnect(c);
}

void debugCallback(valkeyAsyncContext *c, void *r, void *privdata) {
    (void)privdata;
    valkeyReply *reply = r;

    if (reply == NULL) {
        printf("`DEBUG SLEEP` error: %s\n", c->errstr ? c->errstr : "unknown error");
        return;
    }

    valkeyAsyncDisconnect(c);
}

void connectCallback(const valkeyAsyncContext *c, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Connected...\n");
}

void disconnectCallback(const valkeyAsyncContext *c, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Disconnected...\n");
}

int main (int argc, char **argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    valkeyAsyncContext *c = valkeyAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

    hloop_t* loop = hloop_new(HLOOP_FLAG_QUIT_WHEN_NO_ACTIVE_EVENTS);
    valkeyLibhvAttach(c, loop);
    valkeyAsyncSetTimeout(c, (struct timeval){.tv_sec = 0, .tv_usec = 500000});
    valkeyAsyncSetConnectCallback(c,connectCallback);
    valkeyAsyncSetDisconnectCallback(c,disconnectCallback);
    valkeyAsyncCommand(c, NULL, NULL, "SET key %b", argv[argc-1], strlen(argv[argc-1]));
    valkeyAsyncCommand(c, getCallback, (char*)"end-1", "GET key");
    valkeyAsyncCommand(c, debugCallback, NULL, "DEBUG SLEEP %d", 1);
    hloop_run(loop);
    hloop_free(&loop);
    return 0;
}
