#include <valkey/async.h>

#include <valkey/adapters/poll.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static int exit_loop = 0;
static int final_status = 1;
static double debug_sleep_seconds = 2.0;

static void debugSleepCallback(valkeyAsyncContext *ac, void *reply, void *privdata) {
    (void)privdata;

    if (reply == NULL) {
        printf("DEBUG SLEEP callback received NULL reply: %s\n",
               ac->errstr ? ac->errstr : "unknown error");
        return;
    }

    valkeyReply *r = reply;
    printf("DEBUG SLEEP %.3g completed with reply type %d\n",
           debug_sleep_seconds, r->type);
    printf("The disabled command timeout did not tear down the connection.\n");
    final_status = 0;
    valkeyAsyncDisconnect(ac);
}

static void connectCallback(valkeyAsyncContext *ac, int status) {
    if (status != VALKEY_OK) {
        printf("Connect failed: %s\n", ac->errstr);
        exit_loop = 1;
        return;
    }

    printf("Connected. Arming a 500ms command timeout.\n");
    if (valkeyAsyncSetTimeout(ac, (struct timeval){.tv_sec = 0, .tv_usec = 500000}) != VALKEY_OK) {
        printf("valkeyAsyncSetTimeout failed: %s\n", ac->errstr);
        exit_loop = 1;
        return;
    }

    printf("Queueing DEBUG SLEEP %.3g; this arms the internal command timer.\n",
           debug_sleep_seconds);
    if (valkeyAsyncCommand(ac, debugSleepCallback, NULL, "DEBUG SLEEP %f",
                           debug_sleep_seconds) != VALKEY_OK) {
        printf("valkeyAsyncCommand failed: %s\n", ac->errstr);
        exit_loop = 1;
        return;
    }

    printf("Disabling the timeout with valkeyAsyncSetTimeout({0,0}).\n");
    if (valkeyAsyncSetTimeout(ac, (struct timeval){0, 0}) != VALKEY_OK) {
        printf("valkeyAsyncSetTimeout disable failed: %s\n", ac->errstr);
        exit_loop = 1;
    }
}

static void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    exit_loop = 1;
    if (status != VALKEY_OK) {
        printf("Disconnected with error: %s\n", ac->errstr);
        return;
    }

    printf("Disconnected cleanly.\n");
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 9999;

    if (argc > 1)
        port = atoi(argv[1]);
    if (argc > 2)
        debug_sleep_seconds = atof(argv[2]);

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    printf("Using %s:%d. Expected old behavior: timeout after ~500ms.\n", host, port);
    printf("Expected fixed behavior: DEBUG SLEEP reply after %.3g seconds.\n",
           debug_sleep_seconds);

    valkeyAsyncContext *ac = valkeyAsyncConnect(host, port);
    if (ac == NULL) {
        printf("valkeyAsyncConnect returned NULL\n");
        return 1;
    }
    if (ac->err) {
        printf("Connect setup failed: %s\n", ac->errstr);
        return 1;
    }

    if (valkeyPollAttach(ac) != VALKEY_OK) {
        printf("valkeyPollAttach failed\n");
        valkeyAsyncFree(ac);
        return 1;
    }

    valkeyAsyncSetConnectCallback(ac, connectCallback);
    valkeyAsyncSetDisconnectCallback(ac, disconnectCallback);

    while (!exit_loop)
        valkeyPollTick(ac, 0.05);

    return final_status;
}
