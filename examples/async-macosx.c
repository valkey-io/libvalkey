//
//  Created by Дмитрий Бахвалов on 13.07.15.
//  Copyright (c) 2015 Dmitry Bakhvalov. All rights reserved.
//

#include <stdio.h>

#include <valkey/valkey.h>
#include <valkey/async.h>
#include <valkey/adapters/macosx.h>

void getCallback(valkeyAsyncContext *c, void *r, void *privdata) {
    valkeyReply *reply = r;
    if (reply == NULL) return;
    printf("argv[%s]: %s\n", (char*)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
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
    CFRunLoopStop(CFRunLoopGetCurrent());
    printf("Disconnected...\n");
}

int main (int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    CFRunLoopRef loop = CFRunLoopGetCurrent();
    if( !loop ) {
        printf("Error: Cannot get current run loop\n");
        return 1;
    }

    valkeyAsyncContext *c = valkeyAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

    valkeyMacOSAttach(c, loop);

    valkeyAsyncSetConnectCallback(c,connectCallback);
    valkeyAsyncSetDisconnectCallback(c,disconnectCallback);

    valkeyAsyncCommand(c, NULL, NULL, "SET key %b", argv[argc-1], strlen(argv[argc-1]));
    valkeyAsyncCommand(c, getCallback, (char*)"end-1", "GET key");

    CFRunLoopRun();

    return 0;
}

