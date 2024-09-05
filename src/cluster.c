/*
 * Copyright (c) 2015-2017, Ieshen Zheng <ieshen.zheng at 163 dot com>
 * Copyright (c) 2020, Nick <heronr1 at gmail dot com>
 * Copyright (c) 2020-2021, Bjorn Svensson <bjorn.a.svensson at est dot tech>
 * Copyright (c) 2020-2021, Viktor Söderqvist <viktor.soderqvist at est dot tech>
 * Copyright (c) 2021, Red Hat
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define _XOPEN_SOURCE 600
#include "win32.h"

#include "cluster.h"

#include "adlist.h"
#include "alloc.h"
#include "command.h"
#include "dict.h"
#include "sds.h"
#include "vkutil.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cluster errors are offset by 100 to be sufficiently out of range of
// standard Valkey errors
#define VALKEY_ERR_CLUSTER_TOO_MANY_RETRIES 100

#define VALKEY_ERROR_MOVED "MOVED"
#define VALKEY_ERROR_ASK "ASK"
#define VALKEY_ERROR_TRYAGAIN "TRYAGAIN"
#define VALKEY_ERROR_CLUSTERDOWN "CLUSTERDOWN"

#define VALKEY_STATUS_OK "OK"

#define VALKEY_COMMAND_CLUSTER_NODES "CLUSTER NODES"
#define VALKEY_COMMAND_CLUSTER_SLOTS "CLUSTER SLOTS"
#define VALKEY_COMMAND_ASKING "ASKING"

#define IP_PORT_SEPARATOR ':'

#define PORT_CPORT_SEPARATOR '@'

#define CLUSTER_ADDRESS_SEPARATOR ","

#define CLUSTER_DEFAULT_MAX_RETRY_COUNT 5
#define NO_RETRY -1

#define CRLF "\x0d\x0a"
#define CRLF_LEN (sizeof("\x0d\x0a") - 1)

#define SLOTMAP_UPDATE_THROTTLE_USEC 1000000
#define SLOTMAP_UPDATE_ONGOING INT64_MAX

typedef struct cluster_async_data {
    valkeyClusterAsyncContext *acc;
    struct cmd *command;
    valkeyClusterCallbackFn *callback;
    int retry_count;
    void *privdata;
} cluster_async_data;

typedef enum CLUSTER_ERR_TYPE {
    CLUSTER_NOT_ERR = 0,
    CLUSTER_ERR_MOVED,
    CLUSTER_ERR_ASK,
    CLUSTER_ERR_TRYAGAIN,
    CLUSTER_ERR_CLUSTERDOWN,
    CLUSTER_ERR_SENTINEL
} CLUSTER_ERR_TYPE;

static void freeValkeyClusterNode(valkeyClusterNode *node);
static void cluster_slot_destroy(cluster_slot *slot);
static int updateNodesAndSlotmap(valkeyClusterContext *cc, dict *nodes);
static int updateSlotMapAsync(valkeyClusterAsyncContext *acc,
                              valkeyAsyncContext *ac);

void listClusterNodeDestructor(void *val) { freeValkeyClusterNode(val); }

void listClusterSlotDestructor(void *val) { cluster_slot_destroy(val); }

unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, sdslen((char *)key));
}

int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2) {
    int l1, l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

void dictSdsDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

void dictClusterNodeDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    freeValkeyClusterNode(val);
}

/* Cluster node hash table
 * maps node address (1.2.3.4:6379) to a valkeyClusterNode
 * Has ownership of valkeyClusterNode memory
 */
dictType clusterNodesDictType = {
    dictSdsHash,              /* hash function */
    NULL,                     /* key dup */
    NULL,                     /* val dup */
    dictSdsKeyCompare,        /* key compare */
    dictSdsDestructor,        /* key destructor */
    dictClusterNodeDestructor /* val destructor */
};

/* Referenced cluster node hash table
 * maps node id (437c719f5.....) to a valkeyClusterNode
 * No ownership of valkeyClusterNode memory
 */
dictType clusterNodesRefDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    NULL,              /* val dup */
    dictSdsKeyCompare, /* key compare */
    dictSdsDestructor, /* key destructor */
    NULL               /* val destructor */
};

void listCommandFree(void *command) {
    struct cmd *cmd = command;
    command_destroy(cmd);
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
static unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{')
            break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen)
        return crc16(key, keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s + 1; e < keylen; e++)
        if (key[e] == '}')
            break;

    /* No '}' or nothing betweeen {} ? Hash the whole key. */
    if (e == keylen || e == s + 1)
        return crc16(key, keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key + s + 1, e - s - 1) & 0x3FFF;
}

static void valkeyClusterSetError(valkeyClusterContext *cc, int type,
                                  const char *str) {
    size_t len;

    if (cc == NULL) {
        return;
    }

    cc->err = type;
    if (str != NULL) {
        len = strlen(str);
        len = len < (sizeof(cc->errstr) - 1) ? len : (sizeof(cc->errstr) - 1);
        memcpy(cc->errstr, str, len);
        cc->errstr[len] = '\0';
    } else {
        /* Only VALKEY_ERR_IO may lack a description! */
        assert(type == VALKEY_ERR_IO);
        strerror_r(errno, cc->errstr, sizeof(cc->errstr));
    }
}

static int cluster_reply_error_type(valkeyReply *reply) {

    if (reply == NULL) {
        return VALKEY_ERR;
    }

    if (reply->type == VALKEY_REPLY_ERROR) {
        if ((int)strlen(VALKEY_ERROR_MOVED) < reply->len &&
            memcmp(reply->str, VALKEY_ERROR_MOVED,
                   strlen(VALKEY_ERROR_MOVED)) == 0) {
            return CLUSTER_ERR_MOVED;
        } else if ((int)strlen(VALKEY_ERROR_ASK) < reply->len &&
                   memcmp(reply->str, VALKEY_ERROR_ASK,
                          strlen(VALKEY_ERROR_ASK)) == 0) {
            return CLUSTER_ERR_ASK;
        } else if ((int)strlen(VALKEY_ERROR_TRYAGAIN) < reply->len &&
                   memcmp(reply->str, VALKEY_ERROR_TRYAGAIN,
                          strlen(VALKEY_ERROR_TRYAGAIN)) == 0) {
            return CLUSTER_ERR_TRYAGAIN;
        } else if ((int)strlen(VALKEY_ERROR_CLUSTERDOWN) < reply->len &&
                   memcmp(reply->str, VALKEY_ERROR_CLUSTERDOWN,
                          strlen(VALKEY_ERROR_CLUSTERDOWN)) == 0) {
            return CLUSTER_ERR_CLUSTERDOWN;
        } else {
            return CLUSTER_ERR_SENTINEL;
        }
    }

    return CLUSTER_NOT_ERR;
}

/* Create and initiate the cluster node structure */
static valkeyClusterNode *createValkeyClusterNode(void) {
    /* use calloc to guarantee all fields are zeroed */
    return vk_calloc(1, sizeof(valkeyClusterNode));
}

/* Cleanup the cluster node structure */
static void freeValkeyClusterNode(valkeyClusterNode *node) {
    if (node == NULL) {
        return;
    }

    sdsfree(node->name);
    sdsfree(node->addr);
    sdsfree(node->host);
    valkeyFree(node->con);

    if (node->acon != NULL) {
        /* Detach this cluster node from the async context. This makes sure
         * that valkeyAsyncFree() wont attempt to update the pointer via its
         * dataCleanup and unlinkAsyncContextAndNode() */
        node->acon->data = NULL;
        valkeyAsyncFree(node->acon);
    }
    if (node->slots != NULL) {
        listRelease(node->slots);
    }
    if (node->slaves != NULL) {
        listRelease(node->slaves);
    }
    vk_free(node);
}

static cluster_slot *cluster_slot_create(valkeyClusterNode *node) {
    cluster_slot *slot;

    slot = vk_calloc(1, sizeof(*slot));
    if (slot == NULL) {
        return NULL;
    }
    slot->node = node;

    if (node != NULL) {
        assert(node->role == VALKEY_ROLE_MASTER);
        if (node->slots == NULL) {
            node->slots = listCreate();
            if (node->slots == NULL) {
                cluster_slot_destroy(slot);
                return NULL;
            }

            node->slots->free = listClusterSlotDestructor;
        }

        if (listAddNodeTail(node->slots, slot) == NULL) {
            cluster_slot_destroy(slot);
            return NULL;
        }
    }

    return slot;
}

static int cluster_slot_ref_node(cluster_slot *slot, valkeyClusterNode *node) {
    if (slot == NULL || node == NULL) {
        return VALKEY_ERR;
    }

    if (node->role != VALKEY_ROLE_MASTER) {
        return VALKEY_ERR;
    }

    if (node->slots == NULL) {
        node->slots = listCreate();
        if (node->slots == NULL) {
            return VALKEY_ERR;
        }

        node->slots->free = listClusterSlotDestructor;
    }

    if (listAddNodeTail(node->slots, slot) == NULL) {
        return VALKEY_ERR;
    }
    slot->node = node;

    return VALKEY_OK;
}

static void cluster_slot_destroy(cluster_slot *slot) {
    slot->start = 0;
    slot->end = 0;
    slot->node = NULL;

    vk_free(slot);
}

/**
 * Handle password authentication in the synchronous API
 */
static int authenticate(valkeyClusterContext *cc, valkeyContext *c) {
    if (cc == NULL || c == NULL) {
        return VALKEY_ERR;
    }

    // Skip if no password configured
    if (cc->password == NULL) {
        return VALKEY_OK;
    }

    valkeyReply *reply;
    if (cc->username != NULL) {
        reply = valkeyCommand(c, "AUTH %s %s", cc->username, cc->password);
    } else {
        reply = valkeyCommand(c, "AUTH %s", cc->password);
    }

    if (reply == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "Command AUTH reply error (NULL)");
        goto error;
    }

    if (reply->type == VALKEY_REPLY_ERROR) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, reply->str);
        goto error;
    }

    freeReplyObject(reply);
    return VALKEY_OK;

error:
    freeReplyObject(reply);

    return VALKEY_ERR;
}

/**
 * Return a new node with the "cluster slots" command reply.
 */
static valkeyClusterNode *node_get_with_slots(valkeyClusterContext *cc,
                                              valkeyReply *host_elem,
                                              valkeyReply *port_elem,
                                              uint8_t role) {
    valkeyClusterNode *node = NULL;

    if (host_elem == NULL || port_elem == NULL) {
        return NULL;
    }

    if (host_elem->type != VALKEY_REPLY_STRING || host_elem->len <= 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "Command(cluster slots) reply error: "
                              "node ip is not string.");
        goto error;
    }

    if (port_elem->type != VALKEY_REPLY_INTEGER) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "Command(cluster slots) reply error: "
                              "node port is not integer.");
        goto error;
    }

    if (port_elem->integer < 1 || port_elem->integer > UINT16_MAX) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "Command(cluster slots) reply error: "
                              "node port is not valid.");
        goto error;
    }

    node = createValkeyClusterNode();
    if (node == NULL) {
        goto oom;
    }

    if (role == VALKEY_ROLE_MASTER) {
        node->slots = listCreate();
        if (node->slots == NULL) {
            goto oom;
        }

        node->slots->free = listClusterSlotDestructor;
    }

    node->addr = sdsnewlen(host_elem->str, host_elem->len);
    if (node->addr == NULL) {
        goto oom;
    }
    node->addr = sdscatfmt(node->addr, ":%i", port_elem->integer);
    if (node->addr == NULL) {
        goto oom;
    }
    node->host = sdsnewlen(host_elem->str, host_elem->len);
    if (node->host == NULL) {
        goto oom;
    }
    node->name = NULL;
    node->port = (int)port_elem->integer;
    node->role = role;

    return node;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    if (node != NULL) {
        sdsfree(node->addr);
        sdsfree(node->host);
        vk_free(node);
    }
    return NULL;
}

/**
 * Return a new node with the "cluster nodes" command reply.
 */
static valkeyClusterNode *node_get_with_nodes(valkeyClusterContext *cc,
                                              sds *node_infos, int info_count,
                                              uint8_t role) {
    char *p = NULL;
    valkeyClusterNode *node = NULL;

    if (info_count < 8) {
        return NULL;
    }

    node = createValkeyClusterNode();
    if (node == NULL) {
        goto oom;
    }

    if (role == VALKEY_ROLE_MASTER) {
        node->slots = listCreate();
        if (node->slots == NULL) {
            goto oom;
        }

        node->slots->free = listClusterSlotDestructor;
    }

    /* Handle field <id> */
    node->name = node_infos[0];
    node_infos[0] = NULL; /* Ownership moved */

    /* Handle field <ip:port@cport...>
     * Remove @cport... since addr is used as a dict key which should be <ip>:<port> */
    if ((p = strchr(node_infos[1], PORT_CPORT_SEPARATOR)) != NULL) {
        sdsrange(node_infos[1], 0, p - node_infos[1] - 1 /* skip @ */);
    }
    node->addr = node_infos[1];
    node_infos[1] = NULL; /* Ownership moved */

    node->role = role;

    /* Get the ip part */
    if ((p = strrchr(node->addr, IP_PORT_SEPARATOR)) == NULL) {
        valkeyClusterSetError(
            cc, VALKEY_ERR_OTHER,
            "server address is incorrect, port separator missing.");
        goto error;
    }
    node->host = sdsnewlen(node->addr, p - node->addr);
    if (node->host == NULL) {
        goto oom;
    }
    p++; // remove found separator character

    /* Get the port part */
    node->port = vk_atoi(p, strlen(p));

    return node;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    freeValkeyClusterNode(node);
    return NULL;
}

static void cluster_nodes_swap_ctx(dict *nodes_f, dict *nodes_t) {
    dictEntry *de_f, *de_t;
    valkeyClusterNode *node_f, *node_t;
    valkeyContext *c;
    valkeyAsyncContext *ac;

    if (nodes_f == NULL || nodes_t == NULL) {
        return;
    }

    dictIterator di;
    dictInitIterator(&di, nodes_t);

    while ((de_t = dictNext(&di)) != NULL) {
        node_t = dictGetEntryVal(de_t);
        if (node_t == NULL) {
            continue;
        }

        de_f = dictFind(nodes_f, node_t->addr);
        if (de_f == NULL) {
            continue;
        }

        node_f = dictGetEntryVal(de_f);
        if (node_f->con != NULL) {
            c = node_f->con;
            node_f->con = node_t->con;
            node_t->con = c;
        }

        if (node_f->acon != NULL) {
            ac = node_f->acon;
            node_f->acon = node_t->acon;
            node_t->acon = ac;

            node_t->acon->data = node_t;
            if (node_f->acon)
                node_f->acon->data = node_f;
        }
    }
}

static int cluster_master_slave_mapping_with_name(valkeyClusterContext *cc,
                                                  dict **nodes,
                                                  valkeyClusterNode *node,
                                                  sds master_name) {
    int ret;
    dictEntry *di;
    valkeyClusterNode *node_old;
    listNode *lnode;

    if (node == NULL || master_name == NULL) {
        return VALKEY_ERR;
    }

    if (*nodes == NULL) {
        *nodes = dictCreate(&clusterNodesRefDictType, NULL);
        if (*nodes == NULL) {
            goto oom;
        }
    }

    di = dictFind(*nodes, master_name);
    if (di == NULL) {
        sds key = sdsnewlen(master_name, sdslen(master_name));
        if (key == NULL) {
            goto oom;
        }
        ret = dictAdd(*nodes, key, node);
        if (ret != DICT_OK) {
            sdsfree(key);
            goto oom;
        }

    } else {
        node_old = dictGetEntryVal(di);
        if (node_old == NULL) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "dict get value null");
            return VALKEY_ERR;
        }

        if (node->role == VALKEY_ROLE_MASTER &&
            node_old->role == VALKEY_ROLE_MASTER) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "two masters have the same name");
            return VALKEY_ERR;
        } else if (node->role == VALKEY_ROLE_MASTER &&
                   node_old->role == VALKEY_ROLE_SLAVE) {
            if (node->slaves == NULL) {
                node->slaves = listCreate();
                if (node->slaves == NULL) {
                    goto oom;
                }

                node->slaves->free = listClusterNodeDestructor;
            }

            if (node_old->slaves != NULL) {
                node_old->slaves->free = NULL;
                while (listLength(node_old->slaves) > 0) {
                    lnode = listFirst(node_old->slaves);
                    if (listAddNodeHead(node->slaves, lnode->value) == NULL) {
                        goto oom;
                    }
                    listDelNode(node_old->slaves, lnode);
                }
                listRelease(node_old->slaves);
                node_old->slaves = NULL;
            }

            if (listAddNodeHead(node->slaves, node_old) == NULL) {
                goto oom;
            }
            dictSetHashVal(*nodes, di, node);

        } else if (node->role == VALKEY_ROLE_SLAVE) {
            if (node_old->slaves == NULL) {
                node_old->slaves = listCreate();
                if (node_old->slaves == NULL) {
                    goto oom;
                }

                node_old->slaves->free = listClusterNodeDestructor;
            }
            if (listAddNodeTail(node_old->slaves, node) == NULL) {
                goto oom;
            }
        }
    }

    return VALKEY_OK;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    return VALKEY_ERR;
}

/**
 * Parse the "cluster slots" command reply to nodes dict.
 */
static dict *parse_cluster_slots(valkeyClusterContext *cc, valkeyReply *reply,
                                 int flags) {
    int ret;
    cluster_slot *slot = NULL;
    dict *nodes = NULL;
    dictEntry *den;
    valkeyReply *elem_slots;
    valkeyReply *elem_slots_begin, *elem_slots_end;
    valkeyReply *elem_nodes;
    valkeyReply *elem_ip, *elem_port;
    valkeyClusterNode *master = NULL, *slave;
    uint32_t i, idx;

    if (reply == NULL) {
        return NULL;
    }

    nodes = dictCreate(&clusterNodesDictType, NULL);
    if (nodes == NULL) {
        goto oom;
    }

    if (reply->type != VALKEY_REPLY_ARRAY || reply->elements <= 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "Command(cluster slots) reply error: "
                              "reply is not an array.");
        goto error;
    }

    for (i = 0; i < reply->elements; i++) {
        elem_slots = reply->element[i];
        if (elem_slots->type != VALKEY_REPLY_ARRAY ||
            elem_slots->elements < 3) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "Command(cluster slots) reply error: "
                                  "first sub_reply is not an array.");
            goto error;
        }

        slot = cluster_slot_create(NULL);
        if (slot == NULL) {
            goto oom;
        }

        // one slots region
        for (idx = 0; idx < elem_slots->elements; idx++) {
            if (idx == 0) {
                elem_slots_begin = elem_slots->element[idx];
                if (elem_slots_begin->type != VALKEY_REPLY_INTEGER) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                          "Command(cluster slots) reply error: "
                                          "slot begin is not an integer.");
                    goto error;
                }
                slot->start = (int)(elem_slots_begin->integer);
            } else if (idx == 1) {
                elem_slots_end = elem_slots->element[idx];
                if (elem_slots_end->type != VALKEY_REPLY_INTEGER) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                          "Command(cluster slots) reply error: "
                                          "slot end is not an integer.");
                    goto error;
                }

                slot->end = (int)(elem_slots_end->integer);

                if (slot->start > slot->end) {
                    valkeyClusterSetError(
                        cc, VALKEY_ERR_OTHER,
                        "Command(cluster slots) reply error: "
                        "slot begin is bigger than slot end.");
                    goto error;
                }
            } else {
                elem_nodes = elem_slots->element[idx];
                if (elem_nodes->type != VALKEY_REPLY_ARRAY ||
                    elem_nodes->elements < 2) {
                    valkeyClusterSetError(
                        cc, VALKEY_ERR_OTHER,
                        "Command(cluster slots) reply error: "
                        "nodes sub_reply is not an correct array.");
                    goto error;
                }

                elem_ip = elem_nodes->element[0];
                elem_port = elem_nodes->element[1];

                if (elem_ip == NULL || elem_port == NULL ||
                    elem_ip->type != VALKEY_REPLY_STRING ||
                    elem_port->type != VALKEY_REPLY_INTEGER) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                          "Command(cluster slots) reply error: "
                                          "master ip or port is not correct.");
                    goto error;
                }

                // this is master.
                if (idx == 2) {
                    sds address = sdsnewlen(elem_ip->str, elem_ip->len);
                    if (address == NULL) {
                        goto oom;
                    }
                    address = sdscatfmt(address, ":%i", elem_port->integer);
                    if (address == NULL) {
                        goto oom;
                    }

                    den = dictFind(nodes, address);
                    sdsfree(address);
                    // master already exists, break to the next slots region.
                    if (den != NULL) {

                        master = dictGetEntryVal(den);
                        ret = cluster_slot_ref_node(slot, master);
                        if (ret != VALKEY_OK) {
                            goto oom;
                        }

                        slot = NULL;
                        break;
                    }

                    master = node_get_with_slots(cc, elem_ip, elem_port,
                                                 VALKEY_ROLE_MASTER);
                    if (master == NULL) {
                        goto error;
                    }

                    sds key = sdsnewlen(master->addr, sdslen(master->addr));
                    if (key == NULL) {
                        freeValkeyClusterNode(master);
                        goto oom;
                    }

                    ret = dictAdd(nodes, key, master);
                    if (ret != DICT_OK) {
                        sdsfree(key);
                        freeValkeyClusterNode(master);
                        goto oom;
                    }

                    ret = cluster_slot_ref_node(slot, master);
                    if (ret != VALKEY_OK) {
                        goto oom;
                    }

                    slot = NULL;
                } else if (flags & VALKEYCLUSTER_FLAG_ADD_SLAVE) {
                    slave = node_get_with_slots(cc, elem_ip, elem_port,
                                                VALKEY_ROLE_SLAVE);
                    if (slave == NULL) {
                        goto error;
                    }

                    if (master->slaves == NULL) {
                        master->slaves = listCreate();
                        if (master->slaves == NULL) {
                            freeValkeyClusterNode(slave);
                            goto oom;
                        }

                        master->slaves->free = listClusterNodeDestructor;
                    }

                    if (listAddNodeTail(master->slaves, slave) == NULL) {
                        freeValkeyClusterNode(slave);
                        goto oom;
                    }
                }
            }
        }
    }

    return nodes;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    if (nodes != NULL) {
        dictRelease(nodes);
    }
    if (slot != NULL) {
        cluster_slot_destroy(slot);
    }
    return NULL;
}

/**
 * Parse the "cluster nodes" command reply to nodes dict.
 */
static dict *parse_cluster_nodes(valkeyClusterContext *cc, char *str, int str_len,
                                 int flags) {
    int ret;
    dict *nodes = NULL;
    dict *nodes_name = NULL;
    valkeyClusterNode *master, *slave;
    cluster_slot *slot;
    char *pos, *start, *end, *line_start, *line_end;
    char *role;
    int role_len;
    int slot_start, slot_end, slot_ranges_found = 0;
    sds *part = NULL, *slot_start_end = NULL;
    int count_part = 0, count_slot_start_end = 0;
    int k;
    int len;

    nodes = dictCreate(&clusterNodesDictType, NULL);
    if (nodes == NULL) {
        goto oom;
    }

    start = str;
    end = start + str_len;

    line_start = start;

    for (pos = start; pos < end; pos++) {
        if (*pos == '\n') {
            line_end = pos - 1;
            len = line_end - line_start;

            part = sdssplitlen(line_start, len + 1, " ", 1, &count_part);
            if (part == NULL) {
                goto oom;
            }

            if (count_part < 8) {
                valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                      "split cluster nodes error");
                goto error;
            }

            // if the address string starts with ":0", skip this node.
            if (sdslen(part[1]) >= 2 && memcmp(part[1], ":0", 2) == 0) {
                sdsfreesplitres(part, count_part);
                count_part = 0;
                part = NULL;

                start = pos + 1;
                line_start = start;
                pos = start;

                continue;
            }

            if (sdslen(part[2]) >= 7 && memcmp(part[2], "myself,", 7) == 0) {
                role_len = sdslen(part[2]) - 7;
                role = part[2] + 7;
            } else {
                role_len = sdslen(part[2]);
                role = part[2];
            }

            // add master node
            if (role_len >= 6 && memcmp(role, "master", 6) == 0) {
                master = node_get_with_nodes(cc, part, count_part,
                                             VALKEY_ROLE_MASTER);
                if (master == NULL) {
                    goto error;
                }

                sds key = sdsnewlen(master->addr, sdslen(master->addr));
                if (key == NULL) {
                    freeValkeyClusterNode(master);
                    goto oom;
                }

                ret = dictAdd(nodes, key, master);
                if (ret != DICT_OK) {
                    // Key already exists, but possibly an OOM error
                    valkeyClusterSetError(
                        cc, VALKEY_ERR_OTHER,
                        "The address already exists in the nodes");
                    sdsfree(key);
                    freeValkeyClusterNode(master);
                    goto error;
                }

                if (flags & VALKEYCLUSTER_FLAG_ADD_SLAVE) {
                    ret = cluster_master_slave_mapping_with_name(
                        cc, &nodes_name, master, master->name);
                    if (ret != VALKEY_OK) {
                        freeValkeyClusterNode(master);
                        goto error;
                    }
                }

                for (k = 8; k < count_part; k++) {
                    slot_start_end = sdssplitlen(part[k], sdslen(part[k]), "-",
                                                 1, &count_slot_start_end);
                    if (slot_start_end == NULL) {
                        goto oom;
                    }

                    if (count_slot_start_end == 1) {
                        slot_start = vk_atoi(slot_start_end[0],
                                             sdslen(slot_start_end[0]));
                        slot_end = slot_start;
                    } else if (count_slot_start_end == 2) {
                        slot_start = vk_atoi(slot_start_end[0],
                                             sdslen(slot_start_end[0]));
                        slot_end = vk_atoi(slot_start_end[1],
                                           sdslen(slot_start_end[1]));
                    } else {
                        slot_start = -1;
                        slot_end = -1;
                    }

                    sdsfreesplitres(slot_start_end, count_slot_start_end);
                    count_slot_start_end = 0;
                    slot_start_end = NULL;

                    if (slot_start < 0 || slot_end < 0 ||
                        slot_start > slot_end ||
                        slot_end >= VALKEYCLUSTER_SLOTS) {
                        continue;
                    }
                    slot_ranges_found += 1;

                    slot = cluster_slot_create(master);
                    if (slot == NULL) {
                        goto oom;
                    }

                    slot->start = (uint32_t)slot_start;
                    slot->end = (uint32_t)slot_end;
                }

            }
            // add slave node
            else if ((flags & VALKEYCLUSTER_FLAG_ADD_SLAVE) &&
                     (role_len >= 5 && memcmp(role, "slave", 5) == 0)) {
                slave = node_get_with_nodes(cc, part, count_part,
                                            VALKEY_ROLE_SLAVE);
                if (slave == NULL) {
                    goto error;
                }

                ret = cluster_master_slave_mapping_with_name(cc, &nodes_name,
                                                             slave, part[3]);
                if (ret != VALKEY_OK) {
                    freeValkeyClusterNode(slave);
                    goto error;
                }
            }

            sdsfreesplitres(part, count_part);
            count_part = 0;
            part = NULL;

            start = pos + 1;
            line_start = start;
            pos = start;
        }
    }

    if (slot_ranges_found == 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "No slot information");
        goto error;
    }

    if (nodes_name != NULL) {
        dictRelease(nodes_name);
    }

    return nodes;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    sdsfreesplitres(part, count_part);
    sdsfreesplitres(slot_start_end, count_slot_start_end);
    if (nodes != NULL) {
        dictRelease(nodes);
    }
    if (nodes_name != NULL) {
        dictRelease(nodes_name);
    }
    return NULL;
}

/* Sends CLUSTER SLOTS or CLUSTER NODES to the node with context c. */
static int clusterUpdateRouteSendCommand(valkeyClusterContext *cc,
                                         valkeyContext *c) {
    const char *cmd = (cc->flags & VALKEYCLUSTER_FLAG_ROUTE_USE_SLOTS ?
                           VALKEY_COMMAND_CLUSTER_SLOTS :
                           VALKEY_COMMAND_CLUSTER_NODES);
    if (valkeyAppendCommand(c, cmd) != VALKEY_OK) {
        const char *msg = (cc->flags & VALKEYCLUSTER_FLAG_ROUTE_USE_SLOTS ?
                               "Command (cluster slots) send error." :
                               "Command (cluster nodes) send error.");
        valkeyClusterSetError(cc, c->err, msg);
        return VALKEY_ERR;
    }
    /* Flush buffer to socket. */
    if (valkeyBufferWrite(c, NULL) == VALKEY_ERR)
        return VALKEY_ERR;

    return VALKEY_OK;
}

/* Receives and handles a CLUSTER SLOTS reply from node with context c. */
static int handleClusterSlotsReply(valkeyClusterContext *cc, valkeyContext *c) {
    valkeyReply *reply = NULL;
    int result = valkeyGetReply(c, (void **)&reply);
    if (result != VALKEY_OK) {
        if (c->err == VALKEY_ERR_TIMEOUT) {
            valkeyClusterSetError(
                cc, c->err,
                "Command (cluster slots) reply error (socket timeout)");
        } else {
            valkeyClusterSetError(
                cc, VALKEY_ERR_OTHER,
                "Command (cluster slots) reply error (NULL).");
        }
        return VALKEY_ERR;
    } else if (reply->type != VALKEY_REPLY_ARRAY) {
        if (reply->type == VALKEY_REPLY_ERROR) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER, reply->str);
        } else {
            valkeyClusterSetError(
                cc, VALKEY_ERR_OTHER,
                "Command (cluster slots) reply error: type is not array.");
        }
        freeReplyObject(reply);
        return VALKEY_ERR;
    }

    dict *nodes = parse_cluster_slots(cc, reply, cc->flags);
    freeReplyObject(reply);
    return updateNodesAndSlotmap(cc, nodes);
}

/* Receives and handles a CLUSTER NODES reply from node with context c. */
static int handleClusterNodesReply(valkeyClusterContext *cc, valkeyContext *c) {
    valkeyReply *reply = NULL;
    int result = valkeyGetReply(c, (void **)&reply);
    if (result != VALKEY_OK) {
        if (c->err == VALKEY_ERR_TIMEOUT) {
            valkeyClusterSetError(cc, c->err,
                                  "Command (cluster nodes) reply error "
                                  "(socket timeout)");
        } else {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "Command (cluster nodes) reply error "
                                  "(NULL).");
        }
        return VALKEY_ERR;
    } else if (reply->type != VALKEY_REPLY_STRING) {
        if (reply->type == VALKEY_REPLY_ERROR) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER, reply->str);
        } else {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "Command(cluster nodes) reply error: "
                                  "type is not string.");
        }
        freeReplyObject(reply);
        return VALKEY_ERR;
    }

    dict *nodes = parse_cluster_nodes(cc, reply->str, reply->len, cc->flags);
    freeReplyObject(reply);
    return updateNodesAndSlotmap(cc, nodes);
}

/* Receives and handles a CLUSTER SLOTS or CLUSTER NODES reply from node with
 * context c. */
static int clusterUpdateRouteHandleReply(valkeyClusterContext *cc,
                                         valkeyContext *c) {
    if (cc->flags & VALKEYCLUSTER_FLAG_ROUTE_USE_SLOTS) {
        return handleClusterSlotsReply(cc, c);
    } else {
        return handleClusterNodesReply(cc, c);
    }
}

/**
 * Update route with the "cluster nodes" or "cluster slots" command reply.
 */
static int cluster_update_route_by_addr(valkeyClusterContext *cc,
                                        const char *ip, int port) {
    valkeyContext *c = NULL;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (ip == NULL || port <= 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Ip or port error!");
        goto error;
    }

    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    options.connect_timeout = cc->connect_timeout;
    options.command_timeout = cc->command_timeout;

    c = valkeyConnectWithOptions(&options);
    if (c == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    if (cc->on_connect) {
        cc->on_connect(c, c->err ? VALKEY_ERR : VALKEY_OK);
    }

    if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        goto error;
    }

    if (cc->ssl && cc->ssl_init_fn(c, cc->ssl) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        goto error;
    }

    if (authenticate(cc, c) != VALKEY_OK) {
        goto error;
    }

    if (clusterUpdateRouteSendCommand(cc, c) != VALKEY_OK) {
        goto error;
    }

    if (clusterUpdateRouteHandleReply(cc, c) != VALKEY_OK) {
        goto error;
    }

    valkeyFree(c);
    return VALKEY_OK;

error:
    valkeyFree(c);
    return VALKEY_ERR;
}

/* Update known cluster nodes with a new collection of valkeyClusterNodes.
 * Will also update the slot-to-node lookup table for the new nodes. */
static int updateNodesAndSlotmap(valkeyClusterContext *cc, dict *nodes) {
    if (nodes == NULL) {
        return VALKEY_ERR;
    }

    /* Create a slot to valkeyClusterNode lookup table */
    valkeyClusterNode **table;
    table = vk_calloc(VALKEYCLUSTER_SLOTS, sizeof(valkeyClusterNode *));
    if (table == NULL) {
        goto oom;
    }

    dictIterator di;
    dictInitIterator(&di, nodes);

    dictEntry *de;
    while ((de = dictNext(&di))) {
        valkeyClusterNode *master = dictGetEntryVal(de);
        if (master->role != VALKEY_ROLE_MASTER) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "Node role must be master");
            goto error;
        }

        if (master->slots == NULL) {
            continue;
        }

        listIter li;
        listRewind(master->slots, &li);

        listNode *ln;
        while ((ln = listNext(&li))) {
            cluster_slot *slot = listNodeValue(ln);
            if (slot->start > slot->end || slot->end >= VALKEYCLUSTER_SLOTS) {
                valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                      "Slot region for node is invalid");
                goto error;
            }
            for (uint32_t i = slot->start; i <= slot->end; i++) {
                if (table[i] != NULL) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                          "Different node holds same slot");
                    goto error;
                }
                table[i] = master;
            }
        }
    }

    /* Update slot-to-node table before changing cc->nodes since
     * removal of nodes might trigger user callbacks which may
     * send commands, which depend on the slot-to-node table. */
    if (cc->table != NULL) {
        vk_free(cc->table);
    }
    cc->table = table;

    cc->route_version++;

    // Move all libvalkey contexts in cc->nodes to nodes
    cluster_nodes_swap_ctx(cc->nodes, nodes);

    /* Replace cc->nodes before releasing the old dict since
     * the release procedure might access cc->nodes. */
    dict *oldnodes = cc->nodes;
    cc->nodes = nodes;
    if (oldnodes != NULL) {
        dictRelease(oldnodes);
    }
    if (cc->event_callback != NULL) {
        cc->event_callback(cc, VALKEYCLUSTER_EVENT_SLOTMAP_UPDATED,
                           cc->event_privdata);
        if (cc->route_version == 1) {
            /* Special event the first time the slotmap was updated. */
            cc->event_callback(cc, VALKEYCLUSTER_EVENT_READY,
                               cc->event_privdata);
        }
    }
    cc->need_update_route = 0;
    return VALKEY_OK;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough
error:
    vk_free(table);
    dictRelease(nodes);
    return VALKEY_ERR;
}

int valkeyClusterUpdateSlotmap(valkeyClusterContext *cc) {
    int ret;
    int flag_err_not_set = 1;
    valkeyClusterNode *node;
    dictEntry *de;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->nodes == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "no server address");
        return VALKEY_ERR;
    }

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetEntryVal(de);
        if (node == NULL || node->host == NULL) {
            continue;
        }

        ret = cluster_update_route_by_addr(cc, node->host, node->port);
        if (ret == VALKEY_OK) {
            if (cc->err) {
                cc->err = 0;
                memset(cc->errstr, '\0', strlen(cc->errstr));
            }
            return VALKEY_OK;
        }

        flag_err_not_set = 0;
    }

    if (flag_err_not_set) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "no valid server address");
    }

    return VALKEY_ERR;
}

valkeyClusterContext *valkeyClusterContextInit(void) {
    valkeyClusterContext *cc;

    cc = vk_calloc(1, sizeof(valkeyClusterContext));
    if (cc == NULL)
        return NULL;

    cc->max_retry_count = CLUSTER_DEFAULT_MAX_RETRY_COUNT;
    return cc;
}

void valkeyClusterFree(valkeyClusterContext *cc) {

    if (cc == NULL)
        return;

    if (cc->event_callback) {
        cc->event_callback(cc, VALKEYCLUSTER_EVENT_FREE_CONTEXT,
                           cc->event_privdata);
    }

    if (cc->connect_timeout) {
        vk_free(cc->connect_timeout);
        cc->connect_timeout = NULL;
    }

    if (cc->command_timeout) {
        vk_free(cc->command_timeout);
        cc->command_timeout = NULL;
    }

    if (cc->table != NULL) {
        vk_free(cc->table);
        cc->table = NULL;
    }

    if (cc->nodes != NULL) {
        /* Clear cc->nodes before releasing the dict since the release procedure
           might access cc->nodes. When a node and its valkey context are freed
           all pending callbacks are executed. Clearing cc->nodes prevents a pending
           slotmap update command callback to trigger additional slotmap updates. */
        dict *nodes = cc->nodes;
        cc->nodes = NULL;
        dictRelease(nodes);
    }

    if (cc->requests != NULL) {
        listRelease(cc->requests);
    }

    if (cc->username != NULL) {
        vk_free(cc->username);
        cc->username = NULL;
    }

    if (cc->password != NULL) {
        vk_free(cc->password);
        cc->password = NULL;
    }

    vk_free(cc);
}

static valkeyClusterContext *
valkeyClusterConnectInternal(valkeyClusterContext *cc, const char *addrs) {
    if (valkeyClusterSetOptionAddNodes(cc, addrs) != VALKEY_OK) {
        return cc;
    }
    valkeyClusterUpdateSlotmap(cc);
    return cc;
}

valkeyClusterContext *valkeyClusterConnect(const char *addrs, int flags) {
    valkeyClusterContext *cc;

    cc = valkeyClusterContextInit();

    if (cc == NULL) {
        return NULL;
    }

    cc->flags = flags;

    return valkeyClusterConnectInternal(cc, addrs);
}

valkeyClusterContext *valkeyClusterConnectWithTimeout(const char *addrs,
                                                      const struct timeval tv,
                                                      int flags) {
    valkeyClusterContext *cc;

    cc = valkeyClusterContextInit();

    if (cc == NULL) {
        return NULL;
    }

    cc->flags = flags;

    if (cc->connect_timeout == NULL) {
        cc->connect_timeout = vk_malloc(sizeof(struct timeval));
        if (cc->connect_timeout == NULL) {
            return NULL;
        }
    }

    memcpy(cc->connect_timeout, &tv, sizeof(struct timeval));

    return valkeyClusterConnectInternal(cc, addrs);
}

int valkeyClusterSetOptionAddNode(valkeyClusterContext *cc, const char *addr) {
    dictEntry *node_entry;
    valkeyClusterNode *node = NULL;
    int port, ret;
    sds ip = NULL;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->nodes == NULL) {
        cc->nodes = dictCreate(&clusterNodesDictType, NULL);
        if (cc->nodes == NULL) {
            goto oom;
        }
    }

    sds addr_sds = sdsnew(addr);
    if (addr_sds == NULL) {
        goto oom;
    }
    node_entry = dictFind(cc->nodes, addr_sds);
    sdsfree(addr_sds);
    if (node_entry == NULL) {

        char *p;
        if ((p = strrchr(addr, IP_PORT_SEPARATOR)) == NULL) {
            valkeyClusterSetError(
                cc, VALKEY_ERR_OTHER,
                "server address is incorrect, port separator missing.");
            return VALKEY_ERR;
        }
        // p includes separator

        if (p - addr <= 0) { /* length until separator */
            valkeyClusterSetError(
                cc, VALKEY_ERR_OTHER,
                "server address is incorrect, address part missing.");
            return VALKEY_ERR;
        }

        ip = sdsnewlen(addr, p - addr);
        if (ip == NULL) {
            goto oom;
        }
        p++; // remove separator character

        if (strlen(p) <= 0) {
            valkeyClusterSetError(
                cc, VALKEY_ERR_OTHER,
                "server address is incorrect, port part missing.");
            goto error;
        }

        port = vk_atoi(p, strlen(p));
        if (port <= 0) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "server port is incorrect");
            goto error;
        }

        node = createValkeyClusterNode();
        if (node == NULL) {
            goto oom;
        }

        node->addr = sdsnew(addr);
        if (node->addr == NULL) {
            goto oom;
        }

        node->host = ip;
        node->port = port;

        sds key = sdsnewlen(node->addr, sdslen(node->addr));
        if (key == NULL) {
            goto oom;
        }
        ret = dictAdd(cc->nodes, key, node);
        if (ret != DICT_OK) {
            sdsfree(key);
            goto oom;
        }
    }

    return VALKEY_OK;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    sdsfree(ip);
    if (node != NULL) {
        sdsfree(node->addr);
        vk_free(node);
    }
    return VALKEY_ERR;
}

int valkeyClusterSetOptionAddNodes(valkeyClusterContext *cc,
                                   const char *addrs) {
    int ret;
    sds *address = NULL;
    int address_count = 0;
    int i;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    address = sdssplitlen(addrs, strlen(addrs), CLUSTER_ADDRESS_SEPARATOR,
                          strlen(CLUSTER_ADDRESS_SEPARATOR), &address_count);
    if (address == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    if (address_count <= 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "invalid server addresses (example format: "
                              "127.0.0.1:1234,127.0.0.2:5678)");
        sdsfreesplitres(address, address_count);
        return VALKEY_ERR;
    }

    for (i = 0; i < address_count; i++) {
        ret = valkeyClusterSetOptionAddNode(cc, address[i]);
        if (ret != VALKEY_OK) {
            sdsfreesplitres(address, address_count);
            return VALKEY_ERR;
        }
    }

    sdsfreesplitres(address, address_count);

    return VALKEY_OK;
}

/**
 * Configure a username used during authentication, see
 * the Valkey AUTH command.
 * Disabled by default. Can be disabled again by providing an
 * empty string or a null pointer.
 */
int valkeyClusterSetOptionUsername(valkeyClusterContext *cc,
                                   const char *username) {
    if (cc == NULL) {
        return VALKEY_ERR;
    }

    // Disabling option
    if (username == NULL || username[0] == '\0') {
        vk_free(cc->username);
        cc->username = NULL;
        return VALKEY_OK;
    }

    vk_free(cc->username);
    cc->username = vk_strdup(username);
    if (cc->username == NULL) {
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

/**
 * Configure a password used when connecting to password-protected
 * Valkey instances. (See Valkey AUTH command)
 */
int valkeyClusterSetOptionPassword(valkeyClusterContext *cc,
                                   const char *password) {

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    // Disabling use of password
    if (password == NULL || password[0] == '\0') {
        vk_free(cc->password);
        cc->password = NULL;
        return VALKEY_OK;
    }

    vk_free(cc->password);
    cc->password = vk_strdup(password);
    if (cc->password == NULL) {
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

int valkeyClusterSetOptionParseSlaves(valkeyClusterContext *cc) {

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    cc->flags |= VALKEYCLUSTER_FLAG_ADD_SLAVE;

    return VALKEY_OK;
}

int valkeyClusterSetOptionRouteUseSlots(valkeyClusterContext *cc) {

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    cc->flags |= VALKEYCLUSTER_FLAG_ROUTE_USE_SLOTS;

    return VALKEY_OK;
}

int valkeyClusterSetOptionConnectTimeout(valkeyClusterContext *cc,
                                         const struct timeval tv) {

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->connect_timeout == NULL) {
        cc->connect_timeout = vk_malloc(sizeof(struct timeval));
        if (cc->connect_timeout == NULL) {
            valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
            return VALKEY_ERR;
        }
    }

    memcpy(cc->connect_timeout, &tv, sizeof(struct timeval));

    return VALKEY_OK;
}

int valkeyClusterSetOptionTimeout(valkeyClusterContext *cc,
                                  const struct timeval tv) {
    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->command_timeout == NULL ||
        cc->command_timeout->tv_sec != tv.tv_sec ||
        cc->command_timeout->tv_usec != tv.tv_usec) {

        if (cc->command_timeout == NULL) {
            cc->command_timeout = vk_malloc(sizeof(struct timeval));
            if (cc->command_timeout == NULL) {
                valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
                return VALKEY_ERR;
            }
        }

        memcpy(cc->command_timeout, &tv, sizeof(struct timeval));

        /* Set timeout on already connected nodes */
        if (cc->nodes && dictSize(cc->nodes) > 0) {
            dictEntry *de;
            valkeyClusterNode *node;

            dictIterator di;
            dictInitIterator(&di, cc->nodes);

            while ((de = dictNext(&di)) != NULL) {
                node = dictGetEntryVal(de);
                if (node->acon) {
                    valkeyAsyncSetTimeout(node->acon, tv);
                }
                if (node->con && node->con->err == 0) {
                    valkeySetTimeout(node->con, tv);
                }

                if (node->slaves && listLength(node->slaves) > 0) {
                    valkeyClusterNode *slave;
                    listNode *ln;

                    listIter li;
                    listRewind(node->slaves, &li);

                    while ((ln = listNext(&li)) != NULL) {
                        slave = listNodeValue(ln);
                        if (slave->acon) {
                            valkeyAsyncSetTimeout(slave->acon, tv);
                        }
                        if (slave->con && slave->con->err == 0) {
                            valkeySetTimeout(slave->con, tv);
                        }
                    }
                }
            }
        }
    }

    return VALKEY_OK;
}

int valkeyClusterSetOptionMaxRetry(valkeyClusterContext *cc,
                                   int max_retry_count) {
    if (cc == NULL || max_retry_count <= 0) {
        return VALKEY_ERR;
    }

    cc->max_retry_count = max_retry_count;

    return VALKEY_OK;
}

int valkeyClusterConnect2(valkeyClusterContext *cc) {

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->nodes == NULL || dictSize(cc->nodes) == 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "server address not configured");
        return VALKEY_ERR;
    }
    /* Clear a previously set shutdown flag since we allow a
     * reconnection of an async context using this API (legacy). */
    cc->flags &= ~VALKEYCLUSTER_FLAG_DISCONNECTING;

    return valkeyClusterUpdateSlotmap(cc);
}

valkeyContext *valkeyClusterGetValkeyContext(valkeyClusterContext *cc,
                                             valkeyClusterNode *node) {
    valkeyContext *c = NULL;
    if (node == NULL) {
        return NULL;
    }

    c = node->con;
    if (c != NULL) {
        if (c->err) {
            valkeyReconnect(c);

            if (cc->on_connect) {
                cc->on_connect(c, c->err ? VALKEY_ERR : VALKEY_OK);
            }

            if (cc->ssl && cc->ssl_init_fn(c, cc->ssl) != VALKEY_OK) {
                valkeyClusterSetError(cc, c->err, c->errstr);
            }

            authenticate(cc, c); // err and errstr handled in function
        }

        return c;
    }

    if (node->host == NULL || node->port <= 0) {
        return NULL;
    }

    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, node->host, node->port);
    options.connect_timeout = cc->connect_timeout;
    options.command_timeout = cc->command_timeout;

    c = valkeyConnectWithOptions(&options);
    if (c == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    }

    if (cc->on_connect) {
        cc->on_connect(c, c->err ? VALKEY_ERR : VALKEY_OK);
    }

    if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        valkeyFree(c);
        return NULL;
    }

    if (cc->ssl && cc->ssl_init_fn(c, cc->ssl) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        valkeyFree(c);
        return NULL;
    }

    if (authenticate(cc, c) != VALKEY_OK) {
        valkeyFree(c);
        return NULL;
    }

    node->con = c;

    return c;
}

static valkeyClusterNode *node_get_by_table(valkeyClusterContext *cc,
                                            uint32_t slot_num) {
    if (cc == NULL) {
        return NULL;
    }

    if (slot_num >= VALKEYCLUSTER_SLOTS) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "invalid slot");
        return NULL;
    }

    if (cc->table == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "slotmap not available");
        return NULL;
    }

    if (cc->table[slot_num] == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "slot not served by any node");
        return NULL;
    }

    return cc->table[slot_num];
}

/* Helper function for the valkeyClusterAppendCommand* family of functions.
 *
 * Write a formatted command to the output buffer. When this family
 * is used, you need to call valkeyGetReply yourself to retrieve
 * the reply (or replies in pub/sub).
 */
static int valkeyClusterAppendCommandInternal(valkeyClusterContext *cc,
                                              struct cmd *command) {

    valkeyClusterNode *node;
    valkeyContext *c = NULL;

    if (cc == NULL || command == NULL) {
        return VALKEY_ERR;
    }

    node = node_get_by_table(cc, (uint32_t)command->slot_num);
    if (node == NULL) {
        return VALKEY_ERR;
    }

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL) {
        return VALKEY_ERR;
    } else if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    if (valkeyAppendFormattedCommand(c, command->cmd, command->clen) !=
        VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

/* Helper functions for the valkeyClusterGetReply* family of functions.
 */
static int valkeyClusterGetReplyFromNode(valkeyClusterContext *cc,
                                         valkeyClusterNode *node,
                                         void **reply) {
    valkeyContext *c;

    if (cc == NULL || node == NULL || reply == NULL)
        return VALKEY_ERR;

    c = node->con;
    if (c == NULL) {
        return VALKEY_ERR;
    } else if (c->err) {
        if (cc->need_update_route == 0) {
            cc->retry_count++;
            if (cc->retry_count > cc->max_retry_count) {
                cc->need_update_route = 1;
                cc->retry_count = 0;
            }
        }
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    if (valkeyGetReply(c, reply) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    if (cluster_reply_error_type(*reply) == CLUSTER_ERR_MOVED)
        cc->need_update_route = 1;

    return VALKEY_OK;
}

/* Parses a MOVED or ASK error reply and returns the destination node. The slot
 * is returned by pointer, if provided. */
static valkeyClusterNode *getNodeFromRedirectReply(valkeyClusterContext *cc,
                                                   valkeyReply *reply,
                                                   int *slotptr) {
    valkeyClusterNode *node = NULL;
    sds *part = NULL;
    int part_len = 0;
    char *p;

    /* Expecting ["ASK" | "MOVED", "<slot>", "<endpoint>:<port>"] */
    part = sdssplitlen(reply->str, reply->len, " ", 1, &part_len);
    if (part == NULL) {
        goto oom;
    }
    if (part_len != 3) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "failed to parse redirect");
        goto done;
    }

    /* Parse slot if requested. */
    if (slotptr != NULL) {
        *slotptr = vk_atoi(part[1], sdslen(part[1]));
    }

    /* Find the last occurance of the port separator since
     * IPv6 addresses can contain ':' */
    if ((p = strrchr(part[2], IP_PORT_SEPARATOR)) == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "port separator missing in redirect");
        goto done;
    }
    // p includes separator

    /* Empty endpoint not supported yet */
    if (p - part[2] == 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "endpoint missing in redirect");
        goto done;
    }

    dictEntry *de = dictFind(cc->nodes, part[2]);
    if (de != NULL) {
        node = de->val;
        goto done;
    }

    /* Add this node since it was unknown */
    node = createValkeyClusterNode();
    if (node == NULL) {
        goto oom;
    }
    node->role = VALKEY_ROLE_MASTER;
    node->addr = part[2];
    part[2] = NULL; /* Memory ownership moved */

    node->host = sdsnewlen(node->addr, p - node->addr);
    if (node->host == NULL) {
        goto oom;
    }
    p++; // remove found separator character
    node->port = vk_atoi(p, strlen(p));

    sds key = sdsnewlen(node->addr, sdslen(node->addr));
    if (key == NULL) {
        goto oom;
    }

    if (dictAdd(cc->nodes, key, node) != DICT_OK) {
        sdsfree(key);
        goto oom;
    }

done:
    sdsfreesplitres(part, part_len);
    return node;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    sdsfreesplitres(part, part_len);
    if (node != NULL) {
        sdsfree(node->addr);
        sdsfree(node->host);
        vk_free(node);
    }

    return NULL;
}

static void *valkey_cluster_command_execute(valkeyClusterContext *cc,
                                            struct cmd *command) {
    void *reply = NULL;
    valkeyClusterNode *node;
    valkeyContext *c = NULL;
    int error_type;
    valkeyContext *c_updating_route = NULL;

retry:

    node = node_get_by_table(cc, (uint32_t)command->slot_num);
    if (node == NULL) {
        /* Update the slotmap since the slot is not served. */
        if (valkeyClusterUpdateSlotmap(cc) != VALKEY_OK) {
            goto error;
        }
        node = node_get_by_table(cc, (uint32_t)command->slot_num);
        if (node == NULL) {
            /* Return error since the slot is still not served. */
            goto error;
        }
    }

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL || c->err) {
        /* Failed to connect. Maybe there was a failover and this node is gone.
         * Update slotmap to find out. */
        if (valkeyClusterUpdateSlotmap(cc) != VALKEY_OK) {
            goto error;
        }

        node = node_get_by_table(cc, (uint32_t)command->slot_num);
        if (node == NULL) {
            goto error;
        }
        c = valkeyClusterGetValkeyContext(cc, node);
        if (c == NULL) {
            goto error;
        } else if (c->err) {
            valkeyClusterSetError(cc, c->err, c->errstr);
            goto error;
        }
    }

moved_retry:
ask_retry:

    if (valkeyAppendFormattedCommand(c, command->cmd, command->clen) !=
        VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        goto error;
    }

    /* If update slotmap has been scheduled, do that in the same pipeline. */
    if (cc->need_update_route && c_updating_route == NULL) {
        if (clusterUpdateRouteSendCommand(cc, c) == VALKEY_OK) {
            c_updating_route = c;
        }
    }

    if (valkeyGetReply(c, &reply) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        /* We may need to update the slotmap if this node is removed from the
         * cluster, but the current request may have already timed out so we
         * schedule it for later. */
        if (c->err != VALKEY_ERR_OOM)
            cc->need_update_route = 1;
        goto error;
    }

    error_type = cluster_reply_error_type(reply);
    if (error_type > CLUSTER_NOT_ERR && error_type < CLUSTER_ERR_SENTINEL) {
        cc->retry_count++;
        if (cc->retry_count > cc->max_retry_count) {
            valkeyClusterSetError(cc, VALKEY_ERR_CLUSTER_TOO_MANY_RETRIES,
                                  "too many cluster retries");
            goto error;
        }

        int slot = -1;
        switch (error_type) {
        case CLUSTER_ERR_MOVED:
            node = getNodeFromRedirectReply(cc, reply, &slot);
            freeReplyObject(reply);
            reply = NULL;

            if (node == NULL) {
                /* Failed to parse redirect. Specific error already set. */
                goto error;
            }

            /* Update the slot mapping entry for this slot. */
            if (slot >= 0) {
                cc->table[slot] = node;
            }

            if (c_updating_route == NULL) {
                if (clusterUpdateRouteSendCommand(cc, c) == VALKEY_OK) {
                    /* Deferred update route using the node that sent the
                     * redirect. */
                    c_updating_route = c;
                } else if (valkeyClusterUpdateSlotmap(cc) == VALKEY_OK) {
                    /* Synchronous update route successful using new connection. */
                    cc->err = 0;
                    cc->errstr[0] = '\0';
                } else {
                    /* Failed to update route. Specific error already set. */
                    goto error;
                }
            }

            c = valkeyClusterGetValkeyContext(cc, node);
            if (c == NULL) {
                goto error;
            } else if (c->err) {
                valkeyClusterSetError(cc, c->err, c->errstr);
                goto error;
            }

            goto moved_retry;

            break;
        case CLUSTER_ERR_ASK:
            node = getNodeFromRedirectReply(cc, reply, NULL);
            if (node == NULL) {
                goto error;
            }

            freeReplyObject(reply);
            reply = NULL;

            c = valkeyClusterGetValkeyContext(cc, node);
            if (c == NULL) {
                goto error;
            } else if (c->err) {
                valkeyClusterSetError(cc, c->err, c->errstr);
                goto error;
            }

            reply = valkeyCommand(c, VALKEY_COMMAND_ASKING);
            if (reply == NULL) {
                valkeyClusterSetError(cc, c->err, c->errstr);
                goto error;
            }

            freeReplyObject(reply);
            reply = NULL;

            goto ask_retry;

            break;
        case CLUSTER_ERR_TRYAGAIN:
        case CLUSTER_ERR_CLUSTERDOWN:
            freeReplyObject(reply);
            reply = NULL;
            goto retry;

            break;
        default:

            break;
        }
    }

    goto done;

error:
    if (reply) {
        freeReplyObject(reply);
        reply = NULL;
    }

done:
    if (c_updating_route) {
        /* Deferred CLUSTER SLOTS or CLUSTER NODES in progress. Wait for the
         * reply and handle it. */
        if (clusterUpdateRouteHandleReply(cc, c_updating_route) != VALKEY_OK) {
            /* Clear error and update synchronously using another node. */
            cc->err = 0;
            cc->errstr[0] = '\0';
            if (valkeyClusterUpdateSlotmap(cc) != VALKEY_OK) {
                /* Clear the reply to indicate failure. */
                freeReplyObject(reply);
                reply = NULL;
            }
        }
    }

    return reply;
}

/* Prepare command by parsing the string to find the key and to get the slot. */
static int prepareCommand(valkeyClusterContext *cc, struct cmd *command) {
    if (command->cmd == NULL || command->clen <= 0) {
        return VALKEY_ERR;
    }

    valkey_parse_cmd(command);
    if (command->result == CMD_PARSE_ENOMEM) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }
    if (command->result != CMD_PARSE_OK) {
        valkeyClusterSetError(cc, VALKEY_ERR_PROTOCOL, command->errstr);
        return VALKEY_ERR;
    }
    if (command->key.len == 0) {
        valkeyClusterSetError(
            cc, VALKEY_ERR_OTHER,
            "No keys in command(must have keys for valkey cluster mode)");
        return VALKEY_ERR;
    }
    command->slot_num = keyHashSlot(command->key.start, command->key.len);
    return VALKEY_OK;
}

int valkeyClusterSetConnectCallback(valkeyClusterContext *cc,
                                    void(fn)(const valkeyContext *c,
                                             int status)) {
    if (cc->on_connect == NULL) {
        cc->on_connect = fn;
        return VALKEY_OK;
    }
    return VALKEY_ERR;
}

int valkeyClusterSetEventCallback(valkeyClusterContext *cc,
                                  void(fn)(const valkeyClusterContext *cc,
                                           int event, void *privdata),
                                  void *privdata) {
    if (cc->event_callback == NULL) {
        cc->event_callback = fn;
        cc->event_privdata = privdata;
        return VALKEY_OK;
    }
    return VALKEY_ERR;
}

void *valkeyClusterFormattedCommand(valkeyClusterContext *cc, char *cmd,
                                    int len) {
    valkeyReply *reply = NULL;
    struct cmd *command = NULL;

    if (cc == NULL) {
        return NULL;
    }

    if (cc->err) {
        cc->err = 0;
        memset(cc->errstr, '\0', strlen(cc->errstr));
    }

    command = command_get();
    if (command == NULL) {
        goto oom;
    }
    command->cmd = cmd;
    command->clen = len;

    if (prepareCommand(cc, command) != VALKEY_OK) {
        goto error;
    }

    reply = valkey_cluster_command_execute(cc, command);
    command->cmd = NULL;
    command_destroy(command);
    cc->retry_count = 0;
    return reply;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    if (command != NULL) {
        command->cmd = NULL;
        command_destroy(command);
    }
    cc->retry_count = 0;
    return NULL;
}

void *valkeyClustervCommand(valkeyClusterContext *cc, const char *format,
                            va_list ap) {
    valkeyReply *reply;
    char *cmd;
    int len;

    if (cc == NULL) {
        return NULL;
    }

    len = valkeyvFormatCommand(&cmd, format, ap);

    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    } else if (len == -2) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid format string");
        return NULL;
    }

    reply = valkeyClusterFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return reply;
}

void *valkeyClusterCommand(valkeyClusterContext *cc, const char *format, ...) {
    va_list ap;
    valkeyReply *reply = NULL;

    va_start(ap, format);
    reply = valkeyClustervCommand(cc, format, ap);
    va_end(ap);

    return reply;
}

void *valkeyClustervCommandToNode(valkeyClusterContext *cc,
                                  valkeyClusterNode *node, const char *format,
                                  va_list ap) {
    valkeyContext *c;
    int ret;
    void *reply;
    int updating_slotmap = 0;

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL) {
        return NULL;
    } else if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return NULL;
    }

    if (cc->err) {
        cc->err = 0;
        memset(cc->errstr, '\0', sizeof(cc->errstr));
    }

    ret = valkeyvAppendCommand(c, format, ap);

    if (ret != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return NULL;
    }

    if (cc->need_update_route) {
        /* Pipeline slotmap update on the same connection. */
        if (clusterUpdateRouteSendCommand(cc, c) == VALKEY_OK) {
            updating_slotmap = 1;
        }
    }

    if (valkeyGetReply(c, &reply) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        if (c->err != VALKEY_ERR_OOM)
            cc->need_update_route = 1;
        return NULL;
    }

    if (updating_slotmap) {
        /* Handle reply from pipelined CLUSTER SLOTS or CLUSTER NODES. */
        if (clusterUpdateRouteHandleReply(cc, c) != VALKEY_OK) {
            /* Ignore error. Update will be triggered on the next command. */
            cc->err = 0;
            cc->errstr[0] = '\0';
        }
    }

    return reply;
}

void *valkeyClusterCommandToNode(valkeyClusterContext *cc,
                                 valkeyClusterNode *node, const char *format,
                                 ...) {
    va_list ap;
    valkeyReply *reply = NULL;

    va_start(ap, format);
    reply = valkeyClustervCommandToNode(cc, node, format, ap);
    va_end(ap);

    return reply;
}

void *valkeyClusterCommandArgv(valkeyClusterContext *cc, int argc,
                               const char **argv, const size_t *argvlen) {
    valkeyReply *reply = NULL;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    }

    reply = valkeyClusterFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return reply;
}

int valkeyClusterAppendFormattedCommand(valkeyClusterContext *cc, char *cmd,
                                        int len) {
    struct cmd *command = NULL;

    if (cc->requests == NULL) {
        cc->requests = listCreate();
        if (cc->requests == NULL) {
            goto oom;
        }
        cc->requests->free = listCommandFree;
    }

    command = command_get();
    if (command == NULL) {
        goto oom;
    }
    command->cmd = cmd;
    command->clen = len;

    if (prepareCommand(cc, command) != VALKEY_OK) {
        goto error;
    }

    if (valkeyClusterAppendCommandInternal(cc, command) != VALKEY_OK) {
        goto error;
    }

    command->cmd = NULL;

    if (listAddNodeTail(cc->requests, command) == NULL) {
        goto oom;
    }
    return VALKEY_OK;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    if (command != NULL) {
        command->cmd = NULL;
        command_destroy(command);
    }
    return VALKEY_ERR;
}

int valkeyClustervAppendCommand(valkeyClusterContext *cc, const char *format,
                                va_list ap) {
    int ret;
    char *cmd;
    int len;

    len = valkeyvFormatCommand(&cmd, format, ap);
    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    } else if (len == -2) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid format string");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAppendFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return ret;
}

int valkeyClusterAppendCommand(valkeyClusterContext *cc, const char *format,
                               ...) {

    int ret;
    va_list ap;

    if (cc == NULL || format == NULL) {
        return VALKEY_ERR;
    }

    va_start(ap, format);
    ret = valkeyClustervAppendCommand(cc, format, ap);
    va_end(ap);

    return ret;
}

int valkeyClustervAppendCommandToNode(valkeyClusterContext *cc,
                                      valkeyClusterNode *node,
                                      const char *format, va_list ap) {
    valkeyContext *c;
    struct cmd *command = NULL;
    char *cmd = NULL;
    int len;

    if (cc->requests == NULL) {
        cc->requests = listCreate();
        if (cc->requests == NULL)
            goto oom;

        cc->requests->free = listCommandFree;
    }

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL) {
        return VALKEY_ERR;
    } else if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    len = valkeyvFormatCommand(&cmd, format, ap);

    if (len == -1) {
        goto oom;
    } else if (len == -2) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid format string");
        return VALKEY_ERR;
    }

    // Append the command to the outgoing valkey buffer
    if (valkeyAppendFormattedCommand(c, cmd, len) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        vk_free(cmd);
        return VALKEY_ERR;
    }

    // Keep the command in the outstanding request list
    command = command_get();
    if (command == NULL) {
        vk_free(cmd);
        goto oom;
    }
    command->cmd = cmd;
    command->clen = len;
    command->node_addr = sdsnew(node->addr);
    if (command->node_addr == NULL)
        goto oom;

    if (listAddNodeTail(cc->requests, command) == NULL)
        goto oom;

    return VALKEY_OK;

oom:
    command_destroy(command);
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    return VALKEY_ERR;
}

int valkeyClusterAppendCommandToNode(valkeyClusterContext *cc,
                                     valkeyClusterNode *node,
                                     const char *format, ...) {
    int ret;
    va_list ap;

    if (cc == NULL || node == NULL || format == NULL) {
        return VALKEY_ERR;
    }

    va_start(ap, format);
    ret = valkeyClustervAppendCommandToNode(cc, node, format, ap);
    va_end(ap);

    return ret;
}

int valkeyClusterAppendCommandArgv(valkeyClusterContext *cc, int argc,
                                   const char **argv, const size_t *argvlen) {
    int ret;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAppendFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return ret;
}

VALKEY_UNUSED
static int valkeyClusterSendAll(valkeyClusterContext *cc) {
    dictEntry *de;
    valkeyClusterNode *node;
    valkeyContext *c = NULL;
    int wdone = 0;

    if (cc == NULL || cc->nodes == NULL) {
        return VALKEY_ERR;
    }

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetEntryVal(de);
        if (node == NULL) {
            continue;
        }

        c = valkeyClusterGetValkeyContext(cc, node);
        if (c == NULL) {
            continue;
        }

        /* Write until done */
        do {
            if (valkeyBufferWrite(c, &wdone) == VALKEY_ERR) {
                return VALKEY_ERR;
            }
        } while (!wdone);
    }

    return VALKEY_OK;
}

VALKEY_UNUSED
static int valkeyClusterClearAll(valkeyClusterContext *cc) {
    dictEntry *de;
    valkeyClusterNode *node;
    valkeyContext *c = NULL;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->err) {
        cc->err = 0;
        memset(cc->errstr, '\0', strlen(cc->errstr));
    }

    if (cc->nodes == NULL) {
        return VALKEY_ERR;
    }

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetEntryVal(de);
        if (node == NULL) {
            continue;
        }

        c = node->con;
        if (c == NULL) {
            continue;
        }

        valkeyFree(c);
        node->con = NULL;
    }

    return VALKEY_OK;
}

int valkeyClusterGetReply(valkeyClusterContext *cc, void **reply) {
    struct cmd *command;
    listNode *list_command;
    int slot_num;

    if (cc == NULL || reply == NULL)
        return VALKEY_ERR;

    cc->err = 0;
    cc->errstr[0] = '\0';

    *reply = NULL;

    if (cc->requests == NULL)
        return VALKEY_ERR; // No queued requests

    list_command = listFirst(cc->requests);

    // no more reply
    if (list_command == NULL) {
        *reply = NULL;
        return VALKEY_OK;
    }

    command = list_command->value;
    if (command == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "command in the requests list is null");
        goto error;
    }

    /* Get reply when the command was sent via slot */
    slot_num = command->slot_num;
    if (slot_num >= 0) {
        valkeyClusterNode *node;
        if ((node = node_get_by_table(cc, (uint32_t)slot_num)) == NULL)
            goto error;

        listDelNode(cc->requests, list_command);
        return valkeyClusterGetReplyFromNode(cc, node, reply);
    }
    /* Get reply when the command was sent to a given node */
    if (command->node_addr != NULL) {
        dictEntry *de = dictFind(cc->nodes, command->node_addr);
        if (de == NULL) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "command was sent to a now unknown node");
            goto error;
        }

        listDelNode(cc->requests, list_command);
        return valkeyClusterGetReplyFromNode(cc, dictGetEntryVal(de), reply);
    }

error:
    listDelNode(cc->requests, list_command);
    return VALKEY_ERR;
}

/**
 * Resets cluster state after pipeline.
 * Resets Valkey node connections if pipeline commands were not called beforehand.
 */
void valkeyClusterReset(valkeyClusterContext *cc) {
    int status;
    void *reply;

    if (cc == NULL || cc->nodes == NULL) {
        return;
    }

    if (cc->err) {
        valkeyClusterClearAll(cc);
    } else {
        /* Write/flush each nodes output buffer to socket */
        valkeyClusterSendAll(cc);

        /* Expect a reply for each pipelined request */
        do {
            status = valkeyClusterGetReply(cc, &reply);
            if (status == VALKEY_OK) {
                freeReplyObject(reply);
            } else {
                valkeyClusterClearAll(cc);
                break;
            }
        } while (reply != NULL);
    }

    if (cc->requests) {
        listRelease(cc->requests);
        cc->requests = NULL;
    }

    if (cc->need_update_route) {
        status = valkeyClusterUpdateSlotmap(cc);
        if (status != VALKEY_OK) {
            /* Specific error already set */
            return;
        }
        cc->need_update_route = 0;
    }
}

/*############valkey cluster async############*/

static void valkeyClusterAsyncSetError(valkeyClusterAsyncContext *acc, int type,
                                       const char *str) {

    size_t len;

    acc->err = type;
    if (str != NULL) {
        len = strlen(str);
        len = len < (sizeof(acc->errstr) - 1) ? len : (sizeof(acc->errstr) - 1);
        memcpy(acc->errstr, str, len);
        acc->errstr[len] = '\0';
    } else {
        /* Only VALKEY_ERR_IO may lack a description! */
        assert(type == VALKEY_ERR_IO);
        strerror_r(errno, acc->errstr, sizeof(acc->errstr));
    }
}

static valkeyClusterAsyncContext *
valkeyClusterAsyncInitialize(valkeyClusterContext *cc) {
    valkeyClusterAsyncContext *acc;

    if (cc == NULL) {
        return NULL;
    }

    acc = vk_calloc(1, sizeof(valkeyClusterAsyncContext));
    if (acc == NULL)
        return NULL;

    acc->cc = cc;

    /* We want the error field to be accessible directly instead of requiring
     * an indirection to the valkeyContext struct. */
    // TODO: really needed?
    acc->err = cc->err;
    memcpy(acc->errstr, cc->errstr, 128);

    return acc;
}

static cluster_async_data *cluster_async_data_create(void) {
    /* use calloc to guarantee all fields are zeroed */
    return vk_calloc(1, sizeof(cluster_async_data));
}

static void cluster_async_data_free(cluster_async_data *cad) {
    if (cad == NULL) {
        return;
    }

    command_destroy(cad->command);

    vk_free(cad);
}

static void unlinkAsyncContextAndNode(void *data) {
    valkeyClusterNode *node;

    if (data) {
        node = (valkeyClusterNode *)(data);
        node->acon = NULL;
    }
}

valkeyAsyncContext *
valkeyClusterGetValkeyAsyncContext(valkeyClusterAsyncContext *acc,
                                   valkeyClusterNode *node) {
    valkeyAsyncContext *ac;
    int ret;

    if (node == NULL) {
        return NULL;
    }

    ac = node->acon;
    if (ac != NULL) {
        if (ac->c.err == 0) {
            return ac;
        } else {
            /* The cluster node has a valkey context with errors. Libvalkey
             * will asynchronously destruct the context and unlink it from
             * the cluster node object. Return an error until done.
             * An example scenario is when sending a command from a command
             * callback, which has a NULL reply due to a disconnect. */
            valkeyClusterAsyncSetError(acc, ac->c.err, ac->c.errstr);
            return NULL;
        }
    }

    // No async context exists, perform a connect

    if (node->host == NULL || node->port <= 0) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "node host or port is error");
        return NULL;
    }

    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, node->host, node->port);
    options.connect_timeout = acc->cc->connect_timeout;
    options.command_timeout = acc->cc->command_timeout;

    node->lastConnectionAttempt = vk_usec_now();

    ac = valkeyAsyncConnectWithOptions(&options);
    if (ac == NULL) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    }

    if (ac->err) {
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);
        valkeyAsyncFree(ac);
        return NULL;
    }

    if (acc->cc->ssl &&
        acc->cc->ssl_init_fn(&ac->c, acc->cc->ssl) != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, ac->c.err, ac->c.errstr);
        valkeyAsyncFree(ac);
        return NULL;
    }

    // Authenticate when needed
    if (acc->cc->password != NULL) {
        if (acc->cc->username != NULL) {
            ret = valkeyAsyncCommand(ac, NULL, NULL, "AUTH %s %s",
                                     acc->cc->username, acc->cc->password);
        } else {
            ret = valkeyAsyncCommand(ac, NULL, NULL, "AUTH %s",
                                     acc->cc->password);
        }

        if (ret != VALKEY_OK) {
            valkeyClusterAsyncSetError(acc, ac->c.err, ac->c.errstr);
            valkeyAsyncFree(ac);
            return NULL;
        }
    }

    if (acc->adapter) {
        ret = acc->attach_fn(ac, acc->adapter);
        if (ret != VALKEY_OK) {
            valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                       "Failed to attach event adapter");
            valkeyAsyncFree(ac);
            return NULL;
        }
    }

    if (acc->onConnect) {
        valkeyAsyncSetConnectCallback(ac, acc->onConnect);
    } else if (acc->onConnectNC) {
        valkeyAsyncSetConnectCallbackNC(ac, acc->onConnectNC);
    }

    if (acc->onDisconnect) {
        valkeyAsyncSetDisconnectCallback(ac, acc->onDisconnect);
    }

    ac->data = node;
    ac->dataCleanup = unlinkAsyncContextAndNode;
    node->acon = ac;

    return ac;
}

valkeyClusterAsyncContext *valkeyClusterAsyncContextInit(void) {
    valkeyClusterContext *cc;
    valkeyClusterAsyncContext *acc;

    cc = valkeyClusterContextInit();
    if (cc == NULL) {
        return NULL;
    }

    acc = valkeyClusterAsyncInitialize(cc);
    if (acc == NULL) {
        valkeyClusterFree(cc);
        return NULL;
    }

    return acc;
}

valkeyClusterAsyncContext *valkeyClusterAsyncConnect(const char *addrs,
                                                     int flags) {

    valkeyClusterContext *cc;
    valkeyClusterAsyncContext *acc;

    cc = valkeyClusterConnect(addrs, flags);
    if (cc == NULL) {
        return NULL;
    }

    acc = valkeyClusterAsyncInitialize(cc);
    if (acc == NULL) {
        valkeyClusterFree(cc);
        return NULL;
    }

    return acc;
}

int valkeyClusterAsyncConnect2(valkeyClusterAsyncContext *acc) {
    /* An adapter to an async event library is required. */
    if (acc->adapter == NULL) {
        return VALKEY_ERR;
    }
    return updateSlotMapAsync(acc, NULL /*any node*/);
}

int valkeyClusterAsyncSetConnectCallback(valkeyClusterAsyncContext *acc,
                                         valkeyConnectCallback *fn) {
    if (acc->onConnect != NULL)
        return VALKEY_ERR;
    if (acc->onConnectNC != NULL)
        return VALKEY_ERR;
    acc->onConnect = fn;
    return VALKEY_OK;
}

int valkeyClusterAsyncSetConnectCallbackNC(valkeyClusterAsyncContext *acc,
                                           valkeyConnectCallbackNC *fn) {
    if (acc->onConnectNC != NULL || acc->onConnect != NULL) {
        return VALKEY_ERR;
    }
    acc->onConnectNC = fn;
    return VALKEY_OK;
}

int valkeyClusterAsyncSetDisconnectCallback(valkeyClusterAsyncContext *acc,
                                            valkeyDisconnectCallback *fn) {
    if (acc->onDisconnect == NULL) {
        acc->onDisconnect = fn;
        return VALKEY_OK;
    }
    return VALKEY_ERR;
}

/* Reply callback function for CLUSTER SLOTS */
void clusterSlotsReplyCallback(valkeyAsyncContext *ac, void *r,
                               void *privdata) {
    UNUSED(ac);
    valkeyReply *reply = (valkeyReply *)r;
    valkeyClusterAsyncContext *acc = (valkeyClusterAsyncContext *)privdata;
    acc->lastSlotmapUpdateAttempt = vk_usec_now();

    if (reply == NULL) {
        /* Retry using available nodes */
        updateSlotMapAsync(acc, NULL);
        return;
    }

    valkeyClusterContext *cc = acc->cc;
    dict *nodes = parse_cluster_slots(cc, reply, cc->flags);
    if (updateNodesAndSlotmap(cc, nodes) != VALKEY_OK) {
        /* Ignore failures for now */
    }
}

/* Reply callback function for CLUSTER NODES */
void clusterNodesReplyCallback(valkeyAsyncContext *ac, void *r,
                               void *privdata) {
    UNUSED(ac);
    valkeyReply *reply = (valkeyReply *)r;
    valkeyClusterAsyncContext *acc = (valkeyClusterAsyncContext *)privdata;
    acc->lastSlotmapUpdateAttempt = vk_usec_now();

    if (reply == NULL) {
        /* Retry using available nodes */
        updateSlotMapAsync(acc, NULL);
        return;
    }

    valkeyClusterContext *cc = acc->cc;
    dict *nodes = parse_cluster_nodes(cc, reply->str, reply->len, cc->flags);
    if (updateNodesAndSlotmap(cc, nodes) != VALKEY_OK) {
        /* Ignore failures for now */
    }
}

#define nodeIsConnected(n)                       \
    ((n)->acon != NULL && (n)->acon->err == 0 && \
     (n)->acon->c.flags & VALKEY_CONNECTED)

/* Select a node.
 * Primarily selects a connected node found close to a randomly picked index of
 * all known nodes. The random index should give a more even distribution of
 * selected nodes. If no connected node is found while iterating to this index
 * the remaining nodes are also checked until a connected node is found.
 * If no connected node is found a node for which a connect has not been attempted
 * within throttle-time, and is found near the picked index, is selected.
 */
static valkeyClusterNode *selectNode(dict *nodes) {
    valkeyClusterNode *node, *selected = NULL;
    dictIterator di;
    dictInitIterator(&di, nodes);

    int64_t throttleLimit = vk_usec_now() - SLOTMAP_UPDATE_THROTTLE_USEC;
    unsigned long currentIndex = 0;
    unsigned long checkIndex = random() % dictSize(nodes);

    dictEntry *de;
    while ((de = dictNext(&di)) != NULL) {
        node = dictGetEntryVal(de);

        if (nodeIsConnected(node)) {
            /* Keep any connected node */
            selected = node;
        } else if (node->lastConnectionAttempt < throttleLimit &&
                   (selected == NULL || (currentIndex < checkIndex &&
                                         !nodeIsConnected(selected)))) {
            /* Keep an accepted node when none is yet found, or
               any accepted node until the chosen index is reached */
            selected = node;
        }

        /* Return a found connected node when chosen index is reached. */
        if (currentIndex >= checkIndex && selected != NULL &&
            nodeIsConnected(selected))
            break;
        currentIndex += 1;
    }
    return selected;
}

/* Update the slot map by querying a selected cluster node. If ac is NULL, an
 * arbitrary connected node is selected. */
static int updateSlotMapAsync(valkeyClusterAsyncContext *acc,
                              valkeyAsyncContext *ac) {
    if (acc->lastSlotmapUpdateAttempt == SLOTMAP_UPDATE_ONGOING) {
        /* Don't allow concurrent slot map updates. */
        return VALKEY_ERR;
    }
    if (acc->cc->flags & VALKEYCLUSTER_FLAG_DISCONNECTING) {
        /* No slot map updates during a cluster client disconnect. */
        return VALKEY_ERR;
    }

    if (ac == NULL) {
        if (acc->cc->nodes == NULL) {
            valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "no nodes added");
            goto error;
        }

        valkeyClusterNode *node = selectNode(acc->cc->nodes);
        if (node == NULL) {
            goto error;
        }

        /* Get libvalkey context, connect if needed */
        ac = valkeyClusterGetValkeyAsyncContext(acc, node);
    }
    if (ac == NULL)
        goto error; /* Specific error already set */

    /* Send a command depending of config */
    int status;
    if (acc->cc->flags & VALKEYCLUSTER_FLAG_ROUTE_USE_SLOTS) {
        status = valkeyAsyncCommand(ac, clusterSlotsReplyCallback, acc,
                                    VALKEY_COMMAND_CLUSTER_SLOTS);
    } else {
        status = valkeyAsyncCommand(ac, clusterNodesReplyCallback, acc,
                                    VALKEY_COMMAND_CLUSTER_NODES);
    }

    if (status == VALKEY_OK) {
        acc->lastSlotmapUpdateAttempt = SLOTMAP_UPDATE_ONGOING;
        return VALKEY_OK;
    }

error:
    acc->lastSlotmapUpdateAttempt = vk_usec_now();
    return VALKEY_ERR;
}

/* Start a slotmap update if the throttling allows. */
static void throttledUpdateSlotMapAsync(valkeyClusterAsyncContext *acc,
                                        valkeyAsyncContext *ac) {
    if (acc->lastSlotmapUpdateAttempt != SLOTMAP_UPDATE_ONGOING &&
        (acc->lastSlotmapUpdateAttempt + SLOTMAP_UPDATE_THROTTLE_USEC) <
            vk_usec_now()) {
        updateSlotMapAsync(acc, ac);
    }
}

static void valkeyClusterAsyncCallback(valkeyAsyncContext *ac, void *r,
                                       void *privdata) {
    int ret;
    valkeyReply *reply = r;
    cluster_async_data *cad = privdata;
    valkeyClusterAsyncContext *acc;
    valkeyClusterContext *cc;
    valkeyAsyncContext *ac_retry = NULL;
    int error_type;
    valkeyClusterNode *node;
    struct cmd *command;

    if (cad == NULL) {
        goto error;
    }

    acc = cad->acc;
    if (acc == NULL) {
        goto error;
    }

    cc = acc->cc;
    if (cc == NULL) {
        goto error;
    }

    command = cad->command;
    if (command == NULL) {
        goto error;
    }

    if (reply == NULL) {
        /* Copy reply specific error from libvalkey */
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);

        node = (valkeyClusterNode *)ac->data;
        if (node == NULL)
            goto done; /* Node already removed from topology */

        /* Start a slotmap update when the throttling allows */
        throttledUpdateSlotMapAsync(acc, NULL);
        goto done;
    }

    /* Skip retry handling when not expected, or during a client disconnect. */
    if (cad->retry_count == NO_RETRY || cc->flags & VALKEYCLUSTER_FLAG_DISCONNECTING)
        goto done;

    error_type = cluster_reply_error_type(reply);

    if (error_type > CLUSTER_NOT_ERR && error_type < CLUSTER_ERR_SENTINEL) {
        cad->retry_count++;
        if (cad->retry_count > cc->max_retry_count) {
            cad->retry_count = 0;
            valkeyClusterAsyncSetError(acc, VALKEY_ERR_CLUSTER_TOO_MANY_RETRIES,
                                       "too many cluster retries");
            goto done;
        }

        int slot = -1;
        switch (error_type) {
        case CLUSTER_ERR_MOVED:
            /* Initiate slot mapping update using the node that sent MOVED. */
            throttledUpdateSlotMapAsync(acc, ac);

            node = getNodeFromRedirectReply(cc, reply, &slot);
            if (node == NULL) {
                valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
                goto done;
            }
            /* Update the slot mapping entry for this slot. */
            if (slot >= 0) {
                cc->table[slot] = node;
            }
            ac_retry = valkeyClusterGetValkeyAsyncContext(acc, node);

            break;
        case CLUSTER_ERR_ASK:
            node = getNodeFromRedirectReply(cc, reply, NULL);
            if (node == NULL) {
                valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
                goto done;
            }

            ac_retry = valkeyClusterGetValkeyAsyncContext(acc, node);
            if (ac_retry == NULL) {
                /* Specific error already set */
                goto done;
            }

            ret =
                valkeyAsyncCommand(ac_retry, NULL, NULL, VALKEY_COMMAND_ASKING);
            if (ret != VALKEY_OK) {
                goto error;
            }

            break;
        case CLUSTER_ERR_TRYAGAIN:
        case CLUSTER_ERR_CLUSTERDOWN:
            ac_retry = ac;

            break;
        default:

            goto done;
            break;
        }

        goto retry;
    }

done:

    if (acc->err) {
        cad->callback(acc, NULL, cad->privdata);
    } else {
        cad->callback(acc, r, cad->privdata);
    }

    if (cc->err) {
        cc->err = 0;
        memset(cc->errstr, '\0', strlen(cc->errstr));
    }

    if (acc->err) {
        acc->err = 0;
        memset(acc->errstr, '\0', strlen(acc->errstr));
    }

    cluster_async_data_free(cad);

    return;

retry:

    ret = valkeyAsyncFormattedCommand(ac_retry, valkeyClusterAsyncCallback, cad,
                                      command->cmd, command->clen);
    if (ret != VALKEY_OK) {
        goto error;
    }

    return;

error:

    cluster_async_data_free(cad);
}

int valkeyClusterAsyncFormattedCommand(valkeyClusterAsyncContext *acc,
                                       valkeyClusterCallbackFn *fn,
                                       void *privdata, char *cmd, int len) {

    valkeyClusterContext *cc;
    int status = VALKEY_OK;
    valkeyClusterNode *node;
    valkeyAsyncContext *ac;
    struct cmd *command = NULL;
    cluster_async_data *cad = NULL;

    if (acc == NULL) {
        return VALKEY_ERR;
    }

    cc = acc->cc;

    /* Don't accept new commands when the client is about to disconnect. */
    if (cc->flags & VALKEYCLUSTER_FLAG_DISCONNECTING) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "disconnecting");
        return VALKEY_ERR;
    }

    if (cc->err) {
        cc->err = 0;
        memset(cc->errstr, '\0', strlen(cc->errstr));
    }

    if (acc->err) {
        acc->err = 0;
        memset(acc->errstr, '\0', strlen(acc->errstr));
    }

    command = command_get();
    if (command == NULL) {
        goto oom;
    }

    command->cmd = vk_calloc(len, sizeof(*command->cmd));
    if (command->cmd == NULL) {
        goto oom;
    }
    memcpy(command->cmd, cmd, len);
    command->clen = len;

    if (prepareCommand(cc, command) != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
        goto error;
    }

    node = node_get_by_table(cc, (uint32_t)command->slot_num);
    if (node == NULL) {
        /* Initiate a slotmap update since the slot is not served. */
        throttledUpdateSlotMapAsync(acc, NULL);

        /* node_get_by_table() has set the error on cc. */
        valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
        goto error;
    }

    ac = valkeyClusterGetValkeyAsyncContext(acc, node);
    if (ac == NULL) {
        /* Specific error already set */
        goto error;
    }

    cad = cluster_async_data_create();
    if (cad == NULL) {
        goto oom;
    }

    cad->acc = acc;
    cad->command = command;
    command = NULL; /* Memory ownership moved. */
    cad->callback = fn;
    cad->privdata = privdata;

    status = valkeyAsyncFormattedCommand(ac, valkeyClusterAsyncCallback, cad,
                                         cmd, len);
    if (status != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);
        goto error;
    }
    return VALKEY_OK;

oom:
    valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    cluster_async_data_free(cad);
    command_destroy(command);
    return VALKEY_ERR;
}

int valkeyClusterAsyncFormattedCommandToNode(valkeyClusterAsyncContext *acc,
                                             valkeyClusterNode *node,
                                             valkeyClusterCallbackFn *fn,
                                             void *privdata, char *cmd,
                                             int len) {
    valkeyClusterContext *cc = acc->cc;
    valkeyAsyncContext *ac;
    int status;
    cluster_async_data *cad = NULL;
    struct cmd *command = NULL;

    /* Don't accept new commands when the client is about to disconnect. */
    if (cc->flags & VALKEYCLUSTER_FLAG_DISCONNECTING) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "disconnecting");
        return VALKEY_ERR;
    }

    ac = valkeyClusterGetValkeyAsyncContext(acc, node);
    if (ac == NULL) {
        /* Specific error already set */
        return VALKEY_ERR;
    }

    if (cc->err) {
        cc->err = 0;
        memset(cc->errstr, '\0', strlen(cc->errstr));
    }

    if (acc->err) {
        acc->err = 0;
        memset(acc->errstr, '\0', strlen(acc->errstr));
    }

    command = command_get();
    if (command == NULL) {
        goto oom;
    }

    command->cmd = vk_calloc(len, sizeof(*command->cmd));
    if (command->cmd == NULL) {
        goto oom;
    }
    memcpy(command->cmd, cmd, len);
    command->clen = len;

    cad = cluster_async_data_create();
    if (cad == NULL)
        goto oom;

    cad->acc = acc;
    cad->command = command;
    command = NULL; /* Memory ownership moved. */
    cad->callback = fn;
    cad->privdata = privdata;
    cad->retry_count = NO_RETRY;

    status = valkeyAsyncFormattedCommand(ac, valkeyClusterAsyncCallback, cad,
                                         cmd, len);
    if (status != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);
        goto error;
    }

    return VALKEY_OK;

oom:
    valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "Out of memory");
    // passthrough

error:
    cluster_async_data_free(cad);
    command_destroy(command);
    return VALKEY_ERR;
}

int valkeyClustervAsyncCommand(valkeyClusterAsyncContext *acc,
                               valkeyClusterCallbackFn *fn, void *privdata,
                               const char *format, va_list ap) {
    int ret;
    char *cmd;
    int len;

    if (acc == NULL) {
        return VALKEY_ERR;
    }

    len = valkeyvFormatCommand(&cmd, format, ap);
    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    } else if (len == -2) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "Invalid format string");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommand(acc, fn, privdata, cmd, len);

    vk_free(cmd);

    return ret;
}

int valkeyClusterAsyncCommand(valkeyClusterAsyncContext *acc,
                              valkeyClusterCallbackFn *fn, void *privdata,
                              const char *format, ...) {
    int ret;
    va_list ap;

    va_start(ap, format);
    ret = valkeyClustervAsyncCommand(acc, fn, privdata, format, ap);
    va_end(ap);

    return ret;
}

int valkeyClusterAsyncCommandToNode(valkeyClusterAsyncContext *acc,
                                    valkeyClusterNode *node,
                                    valkeyClusterCallbackFn *fn, void *privdata,
                                    const char *format, ...) {
    int ret;
    va_list ap;
    int len;
    char *cmd = NULL;

    /* Allocate cmd and encode the variadic command */
    va_start(ap, format);
    len = valkeyvFormatCommand(&cmd, format, ap);
    va_end(ap);

    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "Out of memory");
        return VALKEY_ERR;
    } else if (len == -2) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "Invalid format string");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommandToNode(acc, node, fn, privdata, cmd,
                                                   len);
    vk_free(cmd);
    return ret;
}

int valkeyClusterAsyncCommandArgv(valkeyClusterAsyncContext *acc,
                                  valkeyClusterCallbackFn *fn, void *privdata,
                                  int argc, const char **argv,
                                  const size_t *argvlen) {
    int ret;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommand(acc, fn, privdata, cmd, len);

    vk_free(cmd);

    return ret;
}

int valkeyClusterAsyncCommandArgvToNode(valkeyClusterAsyncContext *acc,
                                        valkeyClusterNode *node,
                                        valkeyClusterCallbackFn *fn,
                                        void *privdata, int argc,
                                        const char **argv,
                                        const size_t *argvlen) {

    int ret;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommandToNode(acc, node, fn, privdata, cmd,
                                                   len);

    vk_free(cmd);

    return ret;
}

void valkeyClusterAsyncDisconnect(valkeyClusterAsyncContext *acc) {
    valkeyClusterContext *cc;
    valkeyAsyncContext *ac;
    dictEntry *de;
    valkeyClusterNode *node;

    if (acc == NULL) {
        return;
    }

    cc = acc->cc;
    cc->flags |= VALKEYCLUSTER_FLAG_DISCONNECTING;

    if (cc->nodes == NULL) {
        return;
    }

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetEntryVal(de);

        ac = node->acon;

        if (ac == NULL) {
            continue;
        }

        valkeyAsyncDisconnect(ac);
    }
}

void valkeyClusterAsyncFree(valkeyClusterAsyncContext *acc) {
    valkeyClusterContext *cc;

    if (acc == NULL) {
        return;
    }

    cc = acc->cc;
    cc->flags |= VALKEYCLUSTER_FLAG_DISCONNECTING;

    valkeyClusterFree(cc);

    vk_free(acc);
}

/* Initiate an iterator for iterating over current cluster nodes */
void valkeyClusterInitNodeIterator(valkeyClusterNodeIterator *iter,
                                   valkeyClusterContext *cc) {
    iter->cc = cc;
    iter->route_version = cc->route_version;
    dictInitIterator(&iter->di, cc->nodes);
    iter->retries_left = 1;
}

/* Get next node from the iterator
 * The iterator will restart if the routing table is updated
 * before all nodes have been iterated. */
valkeyClusterNode *valkeyClusterNodeNext(valkeyClusterNodeIterator *iter) {
    if (iter->retries_left <= 0)
        return NULL;

    if (iter->route_version != iter->cc->route_version) {
        // The routing table has changed and current iterator
        // is invalid. The nodes dict has been recreated in
        // the cluster context. We need to re-init the dictIter.
        dictInitIterator(&iter->di, iter->cc->nodes);
        iter->route_version = iter->cc->route_version;
        iter->retries_left--;
    }

    dictEntry *de;
    if ((de = dictNext(&iter->di)) != NULL)
        return dictGetEntryVal(de);
    else
        return NULL;
}

/* Get hash slot for given key string, which can include hash tags */
unsigned int valkeyClusterGetSlotByKey(char *key) {
    return keyHashSlot(key, strlen(key));
}

/* Get node that handles given key string, which can include hash tags */
valkeyClusterNode *valkeyClusterGetNodeByKey(valkeyClusterContext *cc,
                                             char *key) {
    return node_get_by_table(cc, keyHashSlot(key, strlen(key)));
}
