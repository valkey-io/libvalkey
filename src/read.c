/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
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

#include "fmacros.h"

#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#include <unistd.h>
#endif
#include "win32.h"

#include "alloc.h"
#include "read.h"

#include <sds.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

/* Initial size of our nested reply stack and how much we grow it when needd */
#define VALKEY_READER_STACK_SIZE 9

static void valkeyReaderSetError(valkeyReader *r, int type, const char *str) {
    size_t len;

    if (r->reply != NULL && r->fn && r->fn->freeObject) {
        r->fn->freeObject(r->reply);
        r->reply = NULL;
    }

    /* Clear input buffer on errors. */
    sdsfree(r->buf);
    r->buf = NULL;
    r->pos = r->len = 0;

    /* Reset task stack. */
    r->ridx = -1;

    /* Set error. */
    r->err = type;
    len = strlen(str);
    len = len < (sizeof(r->errstr) - 1) ? len : (sizeof(r->errstr) - 1);
    memcpy(r->errstr, str, len);
    r->errstr[len] = '\0';
}

static size_t chrtos(char *buf, size_t size, char byte) {
    size_t len = 0;

    switch (byte) {
    case '\\':
    case '"':
        len = snprintf(buf, size, "\"\\%c\"", byte);
        break;
    case '\n':
        len = snprintf(buf, size, "\"\\n\"");
        break;
    case '\r':
        len = snprintf(buf, size, "\"\\r\"");
        break;
    case '\t':
        len = snprintf(buf, size, "\"\\t\"");
        break;
    case '\a':
        len = snprintf(buf, size, "\"\\a\"");
        break;
    case '\b':
        len = snprintf(buf, size, "\"\\b\"");
        break;
    default:
        if (isprint(byte))
            len = snprintf(buf, size, "\"%c\"", byte);
        else
            len = snprintf(buf, size, "\"\\x%02x\"", (unsigned char)byte);
        break;
    }

    return len;
}

static void valkeyReaderSetErrorProtocolByte(valkeyReader *r, char byte) {
    char cbuf[8], sbuf[128];

    chrtos(cbuf, sizeof(cbuf), byte);
    snprintf(sbuf, sizeof(sbuf),
             "Protocol error, got %s as reply type byte", cbuf);
    valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL, sbuf);
}

static void valkeyReaderSetErrorOOM(valkeyReader *r) {
    valkeyReaderSetError(r, VALKEY_ERR_OOM, "Out of memory");
}

static char *readBytes(valkeyReader *r, unsigned int bytes) {
    char *p;
    if (r->len - r->pos >= bytes) {
        p = r->buf + r->pos;
        r->pos += bytes;
        return p;
    }
    return NULL;
}

/* Find pointer to \r\n. */
static char *seekNewline(char *s, size_t len) {
    char *ret;

    /* We cannot match with fewer than 2 bytes */
    if (len < 2)
        return NULL;

    /* Search up to len - 1 characters */
    len--;

    /* Look for the \r */
    while ((ret = memchr(s, '\r', len)) != NULL) {
        if (ret[1] == '\n') {
            /* Found. */
            break;
        }
        /* Continue searching. */
        ret++;
        len -= ret - s;
        s = ret;
    }

    return ret;
}

/* Convert a string into a long long. Returns VALKEY_OK if the string could be
 * parsed into a (non-overflowing) long long, VALKEY_ERR otherwise. The value
 * will be set to the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a long long: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. */
static int string2ll(const char *s, size_t slen, long long *value) {
    const char *p = s;
    size_t plen = 0;
    int negative = 0;
    unsigned long long v;

    if (plen == slen)
        return VALKEY_ERR;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL)
            *value = 0;
        return VALKEY_OK;
    }

    if (p[0] == '-') {
        negative = 1;
        p++;
        plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return VALKEY_ERR;
    }

    /* First digit should be 1-9; otherwise, the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0] - '0';
        p++;
        plen++;
    } else if (p[0] == '0' && slen == 1) {
        *value = 0;
        return VALKEY_OK;
    } else {
        return VALKEY_ERR;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (ULLONG_MAX / 10)) /* Overflow. */
            return VALKEY_ERR;
        v *= 10;

        if (v > (ULLONG_MAX - (p[0] - '0'))) /* Overflow. */
            return VALKEY_ERR;
        v += p[0] - '0';

        p++;
        plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return VALKEY_ERR;

    if (negative) {
        if (v > ((unsigned long long)(-(LLONG_MIN + 1)) + 1)) /* Overflow. */
            return VALKEY_ERR;
        if (value != NULL)
            *value = -v;
    } else {
        if (v > LLONG_MAX) /* Overflow. */
            return VALKEY_ERR;
        if (value != NULL)
            *value = v;
    }
    return VALKEY_OK;
}

static char *readLine(valkeyReader *r, int *_len) {
    char *p, *s;
    int len;

    p = r->buf + r->pos;
    s = seekNewline(p, (r->len - r->pos));
    if (s != NULL) {
        len = s - (r->buf + r->pos);
        r->pos += len + 2; /* skip \r\n */
        if (_len)
            *_len = len;
        return p;
    }
    return NULL;
}

static void moveToNextTask(valkeyReader *r) {
    valkeyReadTask *cur, *prv;
    while (r->ridx >= 0) {
        /* Return a.s.a.p. when the stack is now empty. */
        if (r->ridx == 0) {
            r->ridx--;
            return;
        }

        cur = r->task[r->ridx];
        prv = r->task[r->ridx - 1];
        assert(prv->type == VALKEY_REPLY_ARRAY ||
               prv->type == VALKEY_REPLY_MAP ||
               prv->type == VALKEY_REPLY_ATTR ||
               prv->type == VALKEY_REPLY_SET ||
               prv->type == VALKEY_REPLY_PUSH);
        if (cur->idx == prv->elements - 1) {
            r->ridx--;
        } else {
            /* Reset the type because the next item can be anything */
            assert(cur->idx < prv->elements);
            cur->type = -1;
            cur->elements = -1;
            cur->idx++;
            return;
        }
    }
}

static int processLineItem(valkeyReader *r) {
    valkeyReadTask *cur = r->task[r->ridx];
    void *obj;
    char *p;
    int len;

    if ((p = readLine(r, &len)) != NULL) {
        if (cur->type == VALKEY_REPLY_INTEGER) {
            long long v;

            if (string2ll(p, len, &v) == VALKEY_ERR) {
                valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                     "Bad integer value");
                return VALKEY_ERR;
            }

            if (r->fn && r->fn->createInteger) {
                obj = r->fn->createInteger(cur, v);
            } else {
                obj = (void *)VALKEY_REPLY_INTEGER;
            }
        } else if (cur->type == VALKEY_REPLY_DOUBLE) {
            char buf[326], *eptr;
            double d;

            if ((size_t)len >= sizeof(buf)) {
                valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                     "Double value is too large");
                return VALKEY_ERR;
            }

            memcpy(buf, p, len);
            buf[len] = '\0';

            if (len == 3 && strcasecmp(buf, "inf") == 0) {
                d = INFINITY; /* Positive infinite. */
            } else if (len == 4 && strcasecmp(buf, "-inf") == 0) {
                d = -INFINITY; /* Negative infinite. */
            } else if ((len == 3 && strcasecmp(buf, "nan") == 0) ||
                       (len == 4 && strcasecmp(buf, "-nan") == 0)) {
                d = NAN; /* nan. */
            } else {
                d = strtod((char *)buf, &eptr);
                /* RESP3 only allows "inf", "-inf", and finite values, while
                 * strtod() allows other variations on infinity,
                 * etc. We explicitly handle our two allowed infinite cases and NaN
                 * above, so strtod() should only result in finite values. */
                if (buf[0] == '\0' || eptr != &buf[len] || !isfinite(d)) {
                    valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                         "Bad double value");
                    return VALKEY_ERR;
                }
            }

            if (r->fn && r->fn->createDouble) {
                obj = r->fn->createDouble(cur, d, buf, len);
            } else {
                obj = (void *)VALKEY_REPLY_DOUBLE;
            }
        } else if (cur->type == VALKEY_REPLY_NIL) {
            if (len != 0) {
                valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                     "Bad nil value");
                return VALKEY_ERR;
            }

            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void *)VALKEY_REPLY_NIL;
        } else if (cur->type == VALKEY_REPLY_BOOL) {
            int bval;

            if (len != 1 || !strchr("tTfF", p[0])) {
                valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                     "Bad bool value");
                return VALKEY_ERR;
            }

            bval = p[0] == 't' || p[0] == 'T';
            if (r->fn && r->fn->createBool)
                obj = r->fn->createBool(cur, bval);
            else
                obj = (void *)VALKEY_REPLY_BOOL;
        } else if (cur->type == VALKEY_REPLY_BIGNUM) {
            /* Ensure all characters are decimal digits (with possible leading
             * minus sign). */
            for (int i = 0; i < len; i++) {
                /* XXX Consider: Allow leading '+'? Error on leading '0's? */
                if (i == 0 && p[0] == '-')
                    continue;
                if (p[i] < '0' || p[i] > '9') {
                    valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                         "Bad bignum value");
                    return VALKEY_ERR;
                }
            }
            if (r->fn && r->fn->createString)
                obj = r->fn->createString(cur, p, len);
            else
                obj = (void *)VALKEY_REPLY_BIGNUM;
        } else {
            /* Type will be error or status. */
            for (int i = 0; i < len; i++) {
                if (p[i] == '\r' || p[i] == '\n') {
                    valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                         "Bad simple string value");
                    return VALKEY_ERR;
                }
            }
            if (r->fn && r->fn->createString)
                obj = r->fn->createString(cur, p, len);
            else
                obj = (void *)(uintptr_t)(cur->type);
        }

        if (obj == NULL) {
            valkeyReaderSetErrorOOM(r);
            return VALKEY_ERR;
        }

        /* Set reply if this is the root object. */
        if (r->ridx == 0)
            r->reply = obj;
        moveToNextTask(r);
        return VALKEY_OK;
    }

    return VALKEY_ERR;
}

static int processBulkItem(valkeyReader *r) {
    valkeyReadTask *cur = r->task[r->ridx];
    void *obj = NULL;
    char *p, *s;
    long long len;
    unsigned long bytelen;
    int success = 0;

    p = r->buf + r->pos;
    s = seekNewline(p, r->len - r->pos);
    if (s != NULL) {
        p = r->buf + r->pos;
        bytelen = s - (r->buf + r->pos) + 2; /* include \r\n */

        if (string2ll(p, bytelen - 2, &len) == VALKEY_ERR) {
            valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                 "Bad bulk string length");
            return VALKEY_ERR;
        }

        if (len < -1 || (LLONG_MAX > SIZE_MAX && len > (long long)SIZE_MAX)) {
            valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                 "Bulk string length out of range");
            return VALKEY_ERR;
        }

        if (len == -1) {
            /* The nil object can always be created. */
            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void *)VALKEY_REPLY_NIL;
            success = 1;
        } else {
            /* Only continue when the buffer contains the entire bulk item. */
            bytelen += len + 2; /* include \r\n */
            if (r->pos + bytelen <= r->len) {
                if ((cur->type == VALKEY_REPLY_VERB && len < 4) ||
                    (cur->type == VALKEY_REPLY_VERB && s[5] != ':')) {
                    valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                         "Verbatim string 4 bytes of content type are "
                                         "missing or incorrectly encoded.");
                    return VALKEY_ERR;
                }
                if (r->fn && r->fn->createString)
                    obj = r->fn->createString(cur, s + 2, len);
                else
                    obj = (void *)(uintptr_t)cur->type;
                success = 1;
            }
        }

        /* Proceed when obj was created. */
        if (success) {
            if (obj == NULL) {
                valkeyReaderSetErrorOOM(r);
                return VALKEY_ERR;
            }

            r->pos += bytelen;

            /* Set reply if this is the root object. */
            if (r->ridx == 0)
                r->reply = obj;
            moveToNextTask(r);
            return VALKEY_OK;
        }
    }

    return VALKEY_ERR;
}

static int valkeyReaderGrow(valkeyReader *r) {
    valkeyReadTask **aux;
    int newlen;

    /* Grow our stack size */
    newlen = r->tasks + VALKEY_READER_STACK_SIZE;
    aux = vk_realloc(r->task, sizeof(*r->task) * newlen);
    if (aux == NULL)
        goto oom;

    r->task = aux;

    /* Allocate new tasks */
    for (; r->tasks < newlen; r->tasks++) {
        r->task[r->tasks] = vk_calloc(1, sizeof(**r->task));
        if (r->task[r->tasks] == NULL)
            goto oom;
    }

    return VALKEY_OK;
oom:
    valkeyReaderSetErrorOOM(r);
    return VALKEY_ERR;
}

/* Process the array, map and set types. */
static int processAggregateItem(valkeyReader *r) {
    valkeyReadTask *cur = r->task[r->ridx];
    void *obj;
    char *p;
    long long elements;
    int root = 0, len;

    if (r->ridx == r->tasks - 1) {
        if (valkeyReaderGrow(r) == VALKEY_ERR)
            return VALKEY_ERR;
    }

    if ((p = readLine(r, &len)) != NULL) {
        if (string2ll(p, len, &elements) == VALKEY_ERR) {
            valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                 "Bad multi-bulk length");
            return VALKEY_ERR;
        }

        root = (r->ridx == 0);

        if (elements < -1 || (LLONG_MAX > SIZE_MAX && elements > SIZE_MAX) ||
            (r->maxelements > 0 && elements > r->maxelements)) {
            valkeyReaderSetError(r, VALKEY_ERR_PROTOCOL,
                                 "Multi-bulk length out of range");
            return VALKEY_ERR;
        }

        if (elements == -1) {
            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void *)VALKEY_REPLY_NIL;

            if (obj == NULL) {
                valkeyReaderSetErrorOOM(r);
                return VALKEY_ERR;
            }

            moveToNextTask(r);
        } else {
            if (cur->type == VALKEY_REPLY_MAP || cur->type == VALKEY_REPLY_ATTR)
                elements *= 2;

            if (r->fn && r->fn->createArray)
                obj = r->fn->createArray(cur, elements);
            else
                obj = (void *)(uintptr_t)cur->type;

            if (obj == NULL) {
                valkeyReaderSetErrorOOM(r);
                return VALKEY_ERR;
            }

            /* Modify task stack when there are more than 0 elements. */
            if (elements > 0) {
                cur->elements = elements;
                cur->obj = obj;
                r->ridx++;
                r->task[r->ridx]->type = -1;
                r->task[r->ridx]->elements = -1;
                r->task[r->ridx]->idx = 0;
                r->task[r->ridx]->obj = NULL;
                r->task[r->ridx]->parent = cur;
                r->task[r->ridx]->privdata = r->privdata;
            } else {
                moveToNextTask(r);
            }
        }

        /* Set reply if this is the root object. */
        if (root)
            r->reply = obj;
        return VALKEY_OK;
    }

    return VALKEY_ERR;
}

static int processItem(valkeyReader *r) {
    valkeyReadTask *cur = r->task[r->ridx];
    char *p;

    /* check if we need to read type */
    if (cur->type < 0) {
        if ((p = readBytes(r, 1)) != NULL) {
            switch (p[0]) {
            case '-':
                cur->type = VALKEY_REPLY_ERROR;
                break;
            case '+':
                cur->type = VALKEY_REPLY_STATUS;
                break;
            case ':':
                cur->type = VALKEY_REPLY_INTEGER;
                break;
            case ',':
                cur->type = VALKEY_REPLY_DOUBLE;
                break;
            case '_':
                cur->type = VALKEY_REPLY_NIL;
                break;
            case '$':
                cur->type = VALKEY_REPLY_STRING;
                break;
            case '*':
                cur->type = VALKEY_REPLY_ARRAY;
                break;
            case '%':
                cur->type = VALKEY_REPLY_MAP;
                break;
            case '|':
                cur->type = VALKEY_REPLY_ATTR;
                break;
            case '~':
                cur->type = VALKEY_REPLY_SET;
                break;
            case '#':
                cur->type = VALKEY_REPLY_BOOL;
                break;
            case '=':
                cur->type = VALKEY_REPLY_VERB;
                break;
            case '>':
                cur->type = VALKEY_REPLY_PUSH;
                break;
            case '(':
                cur->type = VALKEY_REPLY_BIGNUM;
                break;
            default:
                valkeyReaderSetErrorProtocolByte(r, *p);
                return VALKEY_ERR;
            }
        } else {
            /* could not consume 1 byte */
            return VALKEY_ERR;
        }
    }

    /* process typed item */
    switch (cur->type) {
    case VALKEY_REPLY_ERROR:
    case VALKEY_REPLY_STATUS:
    case VALKEY_REPLY_INTEGER:
    case VALKEY_REPLY_DOUBLE:
    case VALKEY_REPLY_NIL:
    case VALKEY_REPLY_BOOL:
    case VALKEY_REPLY_BIGNUM:
        return processLineItem(r);
    case VALKEY_REPLY_STRING:
    case VALKEY_REPLY_VERB:
        return processBulkItem(r);
    case VALKEY_REPLY_ARRAY:
    case VALKEY_REPLY_MAP:
    case VALKEY_REPLY_ATTR:
    case VALKEY_REPLY_SET:
    case VALKEY_REPLY_PUSH:
        return processAggregateItem(r);
    default:
        assert(NULL);
        return VALKEY_ERR; /* Avoid warning. */
    }
}

valkeyReader *valkeyReaderCreateWithFunctions(valkeyReplyObjectFunctions *fn) {
    valkeyReader *r;

    r = vk_calloc(1, sizeof(valkeyReader));
    if (r == NULL)
        return NULL;

    r->buf = sdsempty();
    if (r->buf == NULL)
        goto oom;

    r->task = vk_calloc(VALKEY_READER_STACK_SIZE, sizeof(*r->task));
    if (r->task == NULL)
        goto oom;

    for (; r->tasks < VALKEY_READER_STACK_SIZE; r->tasks++) {
        r->task[r->tasks] = vk_calloc(1, sizeof(**r->task));
        if (r->task[r->tasks] == NULL)
            goto oom;
    }

    r->fn = fn;
    r->maxbuf = VALKEY_READER_MAX_BUF;
    r->maxelements = VALKEY_READER_MAX_ARRAY_ELEMENTS;
    r->ridx = -1;

    return r;
oom:
    valkeyReaderFree(r);
    return NULL;
}

void valkeyReaderFree(valkeyReader *r) {
    if (r == NULL)
        return;

    if (r->reply != NULL && r->fn && r->fn->freeObject)
        r->fn->freeObject(r->reply);

    if (r->task) {
        /* We know r->task[i] is allocated if i < r->tasks */
        for (int i = 0; i < r->tasks; i++) {
            vk_free(r->task[i]);
        }

        vk_free(r->task);
    }

    sdsfree(r->buf);
    vk_free(r);
}

int valkeyReaderFeed(valkeyReader *r, const char *buf, size_t len) {
    sds newbuf;

    /* Return early when this reader is in an erroneous state. */
    if (r->err)
        return VALKEY_ERR;

    /* Copy the provided buffer. */
    if (buf != NULL && len >= 1) {
        /* Destroy internal buffer when it is empty and is quite large. */
        if (r->len == 0 && r->maxbuf != 0 && sdsavail(r->buf) > r->maxbuf) {
            sdsfree(r->buf);
            r->buf = sdsempty();
            if (r->buf == 0)
                goto oom;

            r->pos = 0;
        }

        newbuf = sdscatlen(r->buf, buf, len);
        if (newbuf == NULL)
            goto oom;

        r->buf = newbuf;
        r->len = sdslen(r->buf);
    }

    return VALKEY_OK;
oom:
    valkeyReaderSetErrorOOM(r);
    return VALKEY_ERR;
}

int valkeyReaderGetReply(valkeyReader *r, void **reply) {
    /* Default target pointer to NULL. */
    if (reply != NULL)
        *reply = NULL;

    /* Return early when this reader is in an erroneous state. */
    if (r->err)
        return VALKEY_ERR;

    /* When the buffer is empty, there will never be a reply. */
    if (r->len == 0)
        return VALKEY_OK;

    /* Set first item to process when the stack is empty. */
    if (r->ridx == -1) {
        r->task[0]->type = -1;
        r->task[0]->elements = -1;
        r->task[0]->idx = -1;
        r->task[0]->obj = NULL;
        r->task[0]->parent = NULL;
        r->task[0]->privdata = r->privdata;
        r->ridx = 0;
    }

    /* Process items in reply. */
    while (r->ridx >= 0)
        if (processItem(r) != VALKEY_OK)
            break;

    /* Return ASAP when an error occurred. */
    if (r->err)
        return VALKEY_ERR;

    /* Discard part of the buffer when we've consumed at least 1k, to avoid
     * doing unnecessary calls to memmove() in sds.c. */
    if (r->pos >= 1024) {
        /* No length check in Valkeys sdsrange() */
        if (sdslen(r->buf) > SSIZE_MAX)
            return VALKEY_ERR;
        sdsrange(r->buf, r->pos, -1);
        r->pos = 0;
        r->len = sdslen(r->buf);
    }

    /* Emit a reply when there is one. */
    if (r->ridx == -1) {
        if (reply != NULL) {
            *reply = r->reply;
        } else if (r->reply != NULL && r->fn && r->fn->freeObject) {
            r->fn->freeObject(r->reply);
        }
        r->reply = NULL;
    }
    return VALKEY_OK;
}
