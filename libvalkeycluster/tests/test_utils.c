#include "valkeycluster.h"

#include "test_utils.h"
#include <stdlib.h>
#include <string.h>

static int valkey_version_major;
static int valkey_version_minor;

/* Helper to extract Valkey version information. */
#define VALKEY_VERSION_FIELD "valkey_version:"
void load_valkey_version(valkeyClusterContext *cc) {
    valkeyClusterNodeIterator ni;
    valkeyClusterNode *node;
    char *eptr, *s, *e;
    valkeyReply *reply = NULL;

    valkeyClusterInitNodeIterator(&ni, cc);
    if ((node = valkeyClusterNodeNext(&ni)) == NULL)
        goto abort;

    reply = valkeyClusterCommandToNode(cc, node, "INFO");
    if (reply == NULL || cc->err || reply->type != VALKEY_REPLY_STRING)
        goto abort;
    if ((s = strstr(reply->str, VALKEY_VERSION_FIELD)) == NULL)
        goto abort;

    s += strlen(VALKEY_VERSION_FIELD);

    /* We need a field terminator and at least 'x.y.z' (5) bytes of data */
    if ((e = strstr(s, "\r\n")) == NULL || (e - s) < 5)
        goto abort;

    /* Extract version info */
    valkey_version_major = strtol(s, &eptr, 10);
    if (*eptr != '.')
        goto abort;
    valkey_version_minor = strtol(eptr + 1, NULL, 10);

    freeReplyObject(reply);
    return;

abort:
    freeReplyObject(reply);
    fprintf(stderr, "Error: Cannot get Valkey version, aborting..\n");
    exit(1);
}

/* Helper to verify Valkey version information. */
int valkey_version_less_than(int major, int minor) {
    if (valkey_version_major == 0) {
        fprintf(stderr, "Error: Valkey version not loaded, aborting..\n");
        exit(1);
    }

    if (valkey_version_major < major)
        return 1;
    if (valkey_version_major == major && valkey_version_minor < minor)
        return 1;
    return 0;
}
