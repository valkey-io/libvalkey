/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
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

#include "sds.h"

#include "sdsalloc.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int sdsHdrSize(char type) {
    switch (type & SDS_TYPE_MASK) {
    case SDS_TYPE_5:
        return sizeof(struct sdshdr5);
    case SDS_TYPE_8:
        return sizeof(struct sdshdr8);
    case SDS_TYPE_16:
        return sizeof(struct sdshdr16);
    case SDS_TYPE_32:
        return sizeof(struct sdshdr32);
    case SDS_TYPE_64:
        return sizeof(struct sdshdr64);
    }
    return 0;
}

static inline char sdsReqType(size_t string_size) {
    if (string_size < 32)
        return SDS_TYPE_5;
    if (string_size < 0xff)
        return SDS_TYPE_8;
    if (string_size < 0xffff)
        return SDS_TYPE_16;
    if (string_size < 0xffffffff)
        return SDS_TYPE_32;
    return SDS_TYPE_64;
}

/* Create a new sds string with the content specified by the 'init' pointer
 * and 'initlen'.
 * If NULL is used for 'init' the string is initialized with zero bytes.
 *
 * The string is always null-terminated (all the sds strings are, always) so
 * even if you create an sds string with:
 *
 * mystring = sdsnewlen("abc",3);
 *
 * You can print the string with printf() as there is an implicit \0 at the
 * end of the string. However the string is binary safe and can contain
 * \0 characters in the middle, as the length is stored in the sds header. */
sds sdsnewlen(const void *init, size_t initlen) {
    void *sh;
    sds s;
    char type = sdsReqType(initlen);
    /* Empty strings are usually created in order to append. Use type 8
     * since type 5 is not good at this. */
    if (type == SDS_TYPE_5 && initlen == 0)
        type = SDS_TYPE_8;
    int hdrlen = sdsHdrSize(type);
    unsigned char *fp; /* flags pointer. */

    if (hdrlen + initlen + 1 <= initlen)
        return NULL; /* Catch size_t overflow */
    sh = s_malloc(hdrlen + initlen + 1);
    if (sh == NULL)
        return NULL;
    if (!init)
        memset(sh, 0, hdrlen + initlen + 1);
    s = (char *)sh + hdrlen;
    fp = ((unsigned char *)s) - 1;
    switch (type) {
    case SDS_TYPE_5: {
        *fp = type | (initlen << SDS_TYPE_BITS);
        break;
    }
    case SDS_TYPE_8: {
        SDS_HDR_VAR(8, s);
        sh->len = initlen;
        sh->alloc = initlen;
        *fp = type;
        break;
    }
    case SDS_TYPE_16: {
        SDS_HDR_VAR(16, s);
        sh->len = initlen;
        sh->alloc = initlen;
        *fp = type;
        break;
    }
    case SDS_TYPE_32: {
        SDS_HDR_VAR(32, s);
        sh->len = initlen;
        sh->alloc = initlen;
        *fp = type;
        break;
    }
    case SDS_TYPE_64: {
        SDS_HDR_VAR(64, s);
        sh->len = initlen;
        sh->alloc = initlen;
        *fp = type;
        break;
    }
    }
    if (initlen && init)
        memcpy(s, init, initlen);
    s[initlen] = '\0';
    return s;
}

/* Create an empty (zero length) sds string. Even in this case the string
 * always has an implicit null term. */
sds sdsempty(void) {
    return sdsnewlen("", 0);
}

/* Create a new sds string starting from a null terminated C string. */
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/* Duplicate an sds string. */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/* Free an sds string. No operation is performed if 's' is NULL. */
void sdsfree(sds s) {
    if (s == NULL)
        return;
    s_free((char *)s - sdsHdrSize(s[-1]));
}

/* Set the sds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the sds string is hacked manually in some
 * way, like in the following example:
 *
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
void sdsupdatelen(sds s) {
    size_t reallen = strlen(s);
    sdssetlen(s, reallen);
}

/* Modify an sds string in-place to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
void sdsclear(sds s) {
    sdssetlen(s, 0);
    s[0] = '\0';
}

/* Enlarge the free space at the end of the sds string so that the caller
 * is sure that after calling this function can overwrite up to addlen
 * bytes after the end of the string, plus one more byte for nul term.
 *
 * Note: this does not change the *length* of the sds string as returned
 * by sdslen(), but only the free buffer space we have. */
sds sdsMakeRoomFor(sds s, size_t addlen) {
    void *sh, *newsh;
    size_t avail = sdsavail(s);
    size_t len, newlen, reqlen;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen;

    /* Return ASAP if there is enough space left. */
    if (avail >= addlen)
        return s;

    len = sdslen(s);
    sh = (char *)s - sdsHdrSize(oldtype);
    reqlen = newlen = (len + addlen);
    if (newlen <= len)
        return NULL; /* Catch size_t overflow */
    if (newlen < SDS_MAX_PREALLOC)
        newlen *= 2;
    else
        newlen += SDS_MAX_PREALLOC;

    type = sdsReqType(newlen);

    /* Don't use type 5: the user is appending to the string and type 5 is
     * not able to remember empty space, so sdsMakeRoomFor() must be called
     * at every appending operation. */
    if (type == SDS_TYPE_5)
        type = SDS_TYPE_8;

    hdrlen = sdsHdrSize(type);
    if (hdrlen + newlen + 1 <= reqlen)
        return NULL; /* Catch size_t overflow */
    if (oldtype == type) {
        newsh = s_realloc(sh, hdrlen + newlen + 1);
        if (newsh == NULL)
            return NULL;
        s = (char *)newsh + hdrlen;
    } else {
        /* Since the header size changes, need to move the string forward,
         * and can't use realloc */
        newsh = s_malloc(hdrlen + newlen + 1);
        if (newsh == NULL)
            return NULL;
        memcpy((char *)newsh + hdrlen, s, len + 1);
        s_free(sh);
        s = (char *)newsh + hdrlen;
        s[-1] = type;
        sdssetlen(s, len);
    }
    sdssetalloc(s, newlen);
    return s;
}

/* Reallocate the sds string so that it has no free space at the end. The
 * contained string remains not altered, but next concatenation operations
 * will require a reallocation.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdsRemoveFreeSpace(sds s) {
    void *sh, *newsh;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen;
    size_t len = sdslen(s);
    sh = (char *)s - sdsHdrSize(oldtype);

    type = sdsReqType(len);
    hdrlen = sdsHdrSize(type);
    if (oldtype == type) {
        newsh = s_realloc(sh, hdrlen + len + 1);
        if (newsh == NULL)
            return NULL;
        s = (char *)newsh + hdrlen;
    } else {
        newsh = s_malloc(hdrlen + len + 1);
        if (newsh == NULL)
            return NULL;
        memcpy((char *)newsh + hdrlen, s, len + 1);
        s_free(sh);
        s = (char *)newsh + hdrlen;
        s[-1] = type;
        sdssetlen(s, len);
    }
    sdssetalloc(s, len);
    return s;
}

/* Return the total size of the allocation of the specified sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
size_t sdsAllocSize(sds s) {
    size_t alloc = sdsalloc(s);
    return sdsHdrSize(s[-1]) + alloc + 1;
}

/* Return the pointer of the actual SDS allocation (normally SDS strings
 * are referenced by the start of the string buffer). */
void *sdsAllocPtr(sds s) {
    return (void *)(s - sdsHdrSize(s[-1]));
}

/* Increment the sds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * This function is used in order to fix the string length after the
 * user calls sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * Usage example:
 *
 * Using sdsIncrLen() and sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * sds string without copying into an intermediate buffer:
 *
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * sdsIncrLen(s, nread);
 */
void sdsIncrLen(sds s, int incr) {
    unsigned char flags = s[-1];
    size_t len;
    switch (flags & SDS_TYPE_MASK) {
    case SDS_TYPE_5: {
        unsigned char *fp = ((unsigned char *)s) - 1;
        unsigned char oldlen = SDS_TYPE_5_LEN(flags);
        assert((incr > 0 && oldlen + incr < 32) || (incr < 0 && oldlen >= (unsigned int)(-incr)));
        *fp = SDS_TYPE_5 | ((oldlen + incr) << SDS_TYPE_BITS);
        len = oldlen + incr;
        break;
    }
    case SDS_TYPE_8: {
        SDS_HDR_VAR(8, s);
        assert((incr >= 0 && sh->alloc - sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
        len = (sh->len += incr);
        break;
    }
    case SDS_TYPE_16: {
        SDS_HDR_VAR(16, s);
        assert((incr >= 0 && sh->alloc - sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
        len = (sh->len += incr);
        break;
    }
    case SDS_TYPE_32: {
        SDS_HDR_VAR(32, s);
        assert((incr >= 0 && sh->alloc - sh->len >= (unsigned int)incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
        len = (sh->len += incr);
        break;
    }
    case SDS_TYPE_64: {
        SDS_HDR_VAR(64, s);
        assert((incr >= 0 && sh->alloc - sh->len >= (uint64_t)incr) || (incr < 0 && sh->len >= (uint64_t)(-incr)));
        len = (sh->len += incr);
        break;
    }
    default:
        len = 0; /* Just to avoid compilation warnings. */
    }
    s[len] = '\0';
}

/* Grow the sds to have the specified length. Bytes that were not part of
 * the original length of the sds will be set to zero.
 *
 * if the specified length is smaller than the current length, no operation
 * is performed. */
sds sdsgrowzero(sds s, size_t len) {
    size_t curlen = sdslen(s);

    if (len <= curlen)
        return s;
    s = sdsMakeRoomFor(s, len - curlen);
    if (s == NULL)
        return NULL;

    /* Make sure added region doesn't contain garbage */
    memset(s + curlen, 0, (len - curlen + 1)); /* also set trailing \0 byte */
    sdssetlen(s, len);
    return s;
}

/* Append the specified binary-safe string pointed by 't' of 'len' bytes to the
 * end of the specified sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatlen(sds s, const void *t, size_t len) {
    size_t curlen = sdslen(s);

    s = sdsMakeRoomFor(s, len);
    if (s == NULL)
        return NULL;
    memcpy(s + curlen, t, len);
    sdssetlen(s, curlen + len);
    s[curlen + len] = '\0';
    return s;
}

/* Append the specified null terminated C string to the sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/* Append the specified sds 't' to the existing sds 's'.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* Destructively modify the sds string 's' to hold the specified binary
 * safe string pointed by 't' of length 'len' bytes. */
sds sdscpylen(sds s, const char *t, size_t len) {
    if (sdsalloc(s) < len) {
        s = sdsMakeRoomFor(s, len - sdslen(s));
        if (s == NULL)
            return NULL;
    }
    memcpy(s, t, len);
    s[len] = '\0';
    sdssetlen(s, len);
    return s;
}

/* Like sdscpylen() but 't' must be a null-terminated string so that the length
 * of the string is obtained with strlen(). */
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* Return the number of digits of 'v' when converted to string in radix 10.
 * See ll2string() for more information. */
static uint32_t digits10(uint64_t v) {
    if (v < 10)
        return 1;
    if (v < 100)
        return 2;
    if (v < 1000)
        return 3;
    if (v < 1000000000000UL) {
        if (v < 100000000UL) {
            if (v < 1000000) {
                if (v < 10000)
                    return 4;
                return 5 + (v >= 100000);
            }
            return 7 + (v >= 10000000UL);
        }
        if (v < 10000000000UL) {
            return 9 + (v >= 1000000000UL);
        }
        return 11 + (v >= 100000000000UL);
    }
    return 12 + digits10(v / 1000000000000UL);
}

/* Convert a unsigned long long into a string. Returns the number of
 * characters needed to represent the number.
 * If the buffer is not big enough to store the string, 0 is returned.
 *
 * Based on the following article (that apparently does not provide a
 * novel approach but only publicizes an already used technique):
 *
 * https://www.facebook.com/notes/facebook-engineering/three-optimization-tips-for-c/10151361643253920 */
static int ull2string(char *dst, size_t dstlen, unsigned long long value) {
    static const char digits[201] = "0001020304050607080910111213141516171819"
                                    "2021222324252627282930313233343536373839"
                                    "4041424344454647484950515253545556575859"
                                    "6061626364656667686970717273747576777879"
                                    "8081828384858687888990919293949596979899";

    /* Check length. */
    uint32_t length = digits10(value);
    if (length >= dstlen)
        goto err;

    /* Null term. */
    uint32_t next = length - 1;
    dst[next + 1] = '\0';
    while (value >= 100) {
        int const i = (value % 100) * 2;
        value /= 100;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
        next -= 2;
    }

    /* Handle last 1-2 digits. */
    if (value < 10) {
        dst[next] = '0' + (uint32_t)value;
    } else {
        int i = (uint32_t)value * 2;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
    }
    return length;
err:
    /* force add Null termination */
    if (dstlen > 0)
        dst[0] = '\0';
    return 0;
}

/* Convert a long long into a string. Returns the number of
 * characters needed to represent the number.
 * If the buffer is not big enough to store the string, 0 is returned. */
static int ll2string(char *dst, size_t dstlen, long long svalue) {
    unsigned long long value;
    int negative = 0;

    /* The ull2string function with 64bit unsigned integers for simplicity, so
     * we convert the number here and remember if it is negative. */
    if (svalue < 0) {
        if (svalue != LLONG_MIN) {
            value = -svalue;
        } else {
            value = ((unsigned long long)LLONG_MAX) + 1;
        }
        if (dstlen < 2)
            goto err;
        negative = 1;
        dst[0] = '-';
        dst++;
        dstlen--;
    } else {
        value = svalue;
    }

    /* Converts the unsigned long long value to string */
    int length = ull2string(dst, dstlen, value);
    if (length == 0)
        return 0;
    return length + negative;

err:
    /* force add Null termination */
    if (dstlen > 0)
        dst[0] = '\0';
    return 0;
}

/* Create an sds string from a long long value. It is much faster than:
 *
 * sdscatprintf(sdsempty(),"%lld\n", value);
 */
#define SDS_LLSTR_SIZE 21
sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];
    int len = ll2string(buf, sizeof(buf), value);

    return sdsnewlen(buf, len);
}

/* Like sdscatprintf() but gets va_list instead of being variadic. */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt) * 2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = s_malloc(buflen);
        if (buf == NULL)
            return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    while (1) {
        buf[buflen - 2] = '\0';
        va_copy(cpy, ap);
        vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (buf[buflen - 2] != '\0') {
            if (buf != staticbuf)
                s_free(buf);
            buflen *= 2;
            buf = s_malloc(buflen);
            if (buf == NULL)
                return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = sdscat(s, buf);
    if (buf != staticbuf)
        s_free(buf);
    return t;
}

/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sdscatvprintf(s, fmt, ap);
    va_end(ap);
    return t;
}

/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    const char *f = fmt;
    long i;
    va_list ap;

    va_start(ap, fmt);
    i = sdslen(s); /* Position of the next byte to write to dest str. */
    while (*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (sdsavail(s) == 0) {
            s = sdsMakeRoomFor(s, 1);
            if (s == NULL)
                goto fmt_error;
        }

        switch (*f) {
        case '%':
            next = *(f + 1);
            f++;
            switch (next) {
            case 's':
            case 'S':
                str = va_arg(ap, char *);
                l = (next == 's') ? strlen(str) : sdslen(str);
                if (sdsavail(s) < l) {
                    s = sdsMakeRoomFor(s, l);
                    if (s == NULL)
                        goto fmt_error;
                }
                memcpy(s + i, str, l);
                sdsinclen(s, l);
                i += l;
                break;
            case 'i':
            case 'I':
                if (next == 'i')
                    num = va_arg(ap, int);
                else
                    num = va_arg(ap, long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = ll2string(buf, sizeof(buf), num);
                    if (sdsavail(s) < l) {
                        s = sdsMakeRoomFor(s, l);
                        if (s == NULL)
                            goto fmt_error;
                    }
                    memcpy(s + i, buf, l);
                    sdsinclen(s, l);
                    i += l;
                }
                break;
            case 'u':
            case 'U':
                if (next == 'u')
                    unum = va_arg(ap, unsigned int);
                else
                    unum = va_arg(ap, unsigned long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = ull2string(buf, sizeof(buf), unum);
                    if (sdsavail(s) < l) {
                        s = sdsMakeRoomFor(s, l);
                        if (s == NULL)
                            goto fmt_error;
                    }
                    memcpy(s + i, buf, l);
                    sdsinclen(s, l);
                    i += l;
                }
                break;
            default: /* Handle %% and generally %<unknown>. */
                s[i++] = next;
                sdsinclen(s, 1);
                break;
            }
            break;
        default:
            s[i++] = *f;
            sdsinclen(s, 1);
            break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;

fmt_error:
    va_end(ap);
    return NULL;
}

/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminated C string.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "Hello World".
 */
sds sdstrim(sds s, const char *cset) {
    char *end, *sp, *ep;
    size_t len;

    sp = s;
    ep = end = s + sdslen(s) - 1;
    while (sp <= end && strchr(cset, *sp))
        sp++;
    while (ep > sp && strchr(cset, *ep))
        ep--;
    len = (sp > ep) ? 0 : ((ep - sp) + 1);
    if (s != sp)
        memmove(s, sp, len);
    s[len] = '\0';
    sdssetlen(s, len);
    return s;
}

/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * Return value:
 * -1 (error) if sdslen(s) is larger than maximum positive ssize_t value.
 *  0 on success.
 *
 * Example:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
int sdsrange(sds s, ssize_t start, ssize_t end) {
    size_t newlen, len = sdslen(s);
    if (len > SSIZE_MAX)
        return -1;

    if (len == 0)
        return 0;
    if (start < 0) {
        start = len + start;
        if (start < 0)
            start = 0;
    }
    if (end < 0) {
        end = len + end;
        if (end < 0)
            end = 0;
    }
    newlen = (start > end) ? 0 : (end - start) + 1;
    if (newlen != 0) {
        if (start >= (ssize_t)len) {
            newlen = 0;
        } else if (end >= (ssize_t)len) {
            end = len - 1;
            newlen = (start > end) ? 0 : (end - start) + 1;
        }
    } else {
        start = 0;
    }
    if (start && newlen)
        memmove(s, s + start, newlen);
    s[newlen] = 0;
    sdssetlen(s, newlen);
    return 0;
}

/* Apply tolower() to every character of the sds string 's'. */
void sdstolower(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++)
        s[j] = tolower(s[j]);
}

/* Apply toupper() to every character of the sds string 's'. */
void sdstoupper(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++)
        s[j] = toupper(s[j]);
}

/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1, s2, minlen);
    if (cmp == 0)
        return l1 - l2;
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5, start = 0, j;
    sds *tokens;

    if (seplen < 1 || len < 0)
        return NULL;

    tokens = s_malloc(sizeof(sds) * slots);
    if (tokens == NULL)
        return NULL;

    if (len == 0) {
        *count = 0;
        return tokens;
    }
    for (j = 0; j < (len - (seplen - 1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements + 2) {
            sds *newtokens;

            slots *= 2;
            newtokens = s_realloc(tokens, sizeof(sds) * slots);
            if (newtokens == NULL)
                goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
        if ((seplen == 1 && *(s + j) == sep[0]) || (memcmp(s + j, sep, seplen) == 0)) {
            tokens[elements] = sdsnewlen(s + start, j - start);
            if (tokens[elements] == NULL)
                goto cleanup;
            elements++;
            start = j + seplen;
            j = j + seplen - 1; /* skip the separator */
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    tokens[elements] = sdsnewlen(s + start, len - start);
    if (tokens[elements] == NULL)
        goto cleanup;
    elements++;
    *count = elements;
    return tokens;

cleanup: {
    int i;
    for (i = 0; i < elements; i++)
        sdsfree(tokens[i]);
    s_free(tokens);
    *count = 0;
    return NULL;
}
}

/* Free the result returned by sdssplitlen(), or do nothing if 'tokens' is NULL. */
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens)
        return;
    while (count--)
        sdsfree(tokens[count]);
    s_free(tokens);
}

/* Append to the sds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatrepr(sds s, const char *p, size_t len) {
    s = sdscatlen(s, "\"", 1);
    while (len--) {
        switch (*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s, "\\%c", *p);
            break;
        case '\n':
            s = sdscatlen(s, "\\n", 2);
            break;
        case '\r':
            s = sdscatlen(s, "\\r", 2);
            break;
        case '\t':
            s = sdscatlen(s, "\\t", 2);
            break;
        case '\a':
            s = sdscatlen(s, "\\a", 2);
            break;
        case '\b':
            s = sdscatlen(s, "\\b", 2);
            break;
        default:
            if (isprint((int)*p))
                s = sdscatprintf(s, "%c", *p);
            else
                s = sdscatprintf(s, "\\x%02x", (unsigned char)*p);
            break;
        }
        p++;
    }
    return sdscatlen(s, "\"", 1);
}

/* Helper function for sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
int hex_digit_to_int(char c) {
    switch (c) {
    case '0':
        return 0;
    case '1':
        return 1;
    case '2':
        return 2;
    case '3':
        return 3;
    case '4':
        return 4;
    case '5':
        return 5;
    case '6':
        return 6;
    case '7':
        return 7;
    case '8':
        return 8;
    case '9':
        return 9;
    case 'a':
    case 'A':
        return 10;
    case 'b':
    case 'B':
        return 11;
    case 'c':
    case 'C':
        return 12;
    case 'd':
    case 'D':
        return 13;
    case 'e':
    case 'E':
        return 14;
    case 'f':
    case 'F':
        return 15;
    default:
        return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while (1) {
        /* skip blanks */
        while (*p && isspace((int)*p))
            p++;
        if (*p) {
            /* get a token */
            int inq = 0;  /* set to 1 if we are in "quotes" */
            int insq = 0; /* set to 1 if we are in 'single quotes' */
            int done = 0;

            if (current == NULL)
                current = sdsempty();
            while (!done) {
                if (inq) {
                    if (*p == '\\' && *(p + 1) == 'x' &&
                        isxdigit((int)*(p + 2)) &&
                        isxdigit((int)*(p + 3))) {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p + 2)) * 16) +
                               hex_digit_to_int(*(p + 3));
                        current = sdscatlen(current, (char *)&byte, 1);
                        p += 3;
                    } else if (*p == '\\' && *(p + 1)) {
                        char c;

                        p++;
                        switch (*p) {
                        case 'n':
                            c = '\n';
                            break;
                        case 'r':
                            c = '\r';
                            break;
                        case 't':
                            c = '\t';
                            break;
                        case 'b':
                            c = '\b';
                            break;
                        case 'a':
                            c = '\a';
                            break;
                        default:
                            c = *p;
                            break;
                        }
                        current = sdscatlen(current, &c, 1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p + 1) && !isspace((int)*(p + 1)))
                            goto err;
                        done = 1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current, p, 1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p + 1) == '\'') {
                        p++;
                        current = sdscatlen(current, "'", 1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p + 1) && !isspace((int)*(p + 1)))
                            goto err;
                        done = 1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current, p, 1);
                    }
                } else {
                    switch (*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done = 1;
                        break;
                    case '"':
                        inq = 1;
                        break;
                    case '\'':
                        insq = 1;
                        break;
                    default:
                        current = sdscatlen(current, p, 1);
                        break;
                    }
                }
                if (*p)
                    p++;
            }
            /* add the token to the vector */
            {
                char **new_vector = s_realloc(vector, ((*argc) + 1) * sizeof(char *));
                if (new_vector == NULL) {
                    s_free(vector);
                    return NULL;
                }

                vector = new_vector;
                vector[*argc] = current;
                (*argc)++;
                current = NULL;
            }
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL)
                vector = s_malloc(sizeof(void *));
            return vector;
        }
    }

err:
    while ((*argc)--)
        sdsfree(vector[*argc]);
    s_free(vector);
    if (current)
        sdsfree(current);
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * The function returns the sds string pointer, that is always the same
 * as the input pointer since no resize is needed. */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an sds string. */
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc - 1)
            join = sdscat(join, sep);
    }
    return join;
}

/* Like sdsjoin, but joins an array of SDS strings. */
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscatsds(join, argv[j]);
        if (j != argc - 1)
            join = sdscatlen(join, sep, seplen);
    }
    return join;
}

/* Wrappers to the allocators used by SDS. Note that SDS will actually
 * just use the macros defined into sdsalloc.h in order to avoid to pay
 * the overhead of function calls. Here we define these wrappers only for
 * the programs SDS is linked to, if they want to touch the SDS internals
 * even if they use a different allocator. */
void *sds_malloc(size_t size) { return s_malloc(size); }
void *sds_realloc(void *ptr, size_t size) { return s_realloc(ptr, size); }
void sds_free(void *ptr) { s_free(ptr); }

#if defined(SDS_TEST_MAIN)
#include "limits.h"
#include "testhelp.h"

#include <stdio.h>

int sdsTest(void) {
    {
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
                  sdslen(x) == 3 && memcmp(x, "foo\0", 4) == 0);

        sdsfree(x);
        x = sdsnewlen("foo", 2);
        test_cond("Create a string with specified length",
                  sdslen(x) == 2 && memcmp(x, "fo\0", 3) == 0);

        x = sdscat(x, "bar");
        test_cond("Strings concatenation",
                  sdslen(x) == 5 && memcmp(x, "fobar\0", 6) == 0);

        x = sdscpy(x, "a");
        test_cond("sdscpy() against an originally longer string",
                  sdslen(x) == 1 && memcmp(x, "a\0", 2) == 0);

        x = sdscpy(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
                  sdslen(x) == 33 &&
                      memcmp(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0", 33) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(), "%d", 123);
        test_cond("sdscatprintf() seems working in the base case",
                  sdslen(x) == 3 && memcmp(x, "123\0", 4) == 0);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN, LLONG_MAX);
        test_cond("sdscatfmt() seems working in the base case",
                  sdslen(x) == 60 &&
                      memcmp(x, "--Hello Hi! World -9223372036854775808,"
                                "9223372036854775807--",
                             60) == 0);
        printf("[%s]\n", x);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
                  sdslen(x) == 35 &&
                      memcmp(x, "--4294967295,18446744073709551615--", 35) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x, " x");
        test_cond("sdstrim() works when all chars match",
                  sdslen(x) == 0)

            sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x, " ");
        test_cond("sdstrim() works when a single char remains",
                  sdslen(x) == 1 && x[0] == 'x')

            sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x, "xy");
        test_cond("sdstrim() correctly trims characters",
                  sdslen(x) == 4 && memcmp(x, "ciao\0", 5) == 0);

        y = sdsdup(x);
        sdsrange(y, 1, 1);
        test_cond("sdsrange(...,1,1)",
                  sdslen(y) == 1 && memcmp(y, "i\0", 2) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 1, -1);
        test_cond("sdsrange(...,1,-1)",
                  sdslen(y) == 3 && memcmp(y, "iao\0", 4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, -2, -1);
        test_cond("sdsrange(...,-2,-1)",
                  sdslen(y) == 2 && memcmp(y, "ao\0", 3) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 2, 1);
        test_cond("sdsrange(...,2,1)",
                  sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 1, 100);
        test_cond("sdsrange(...,1,100)",
                  sdslen(y) == 3 && memcmp(y, "iao\0", 4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 100, 100);
        test_cond("sdsrange(...,100,100)",
                  sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x, y) > 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x, y) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x, y) < 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r", 7);
        y = sdscatrepr(sdsempty(), x, sdslen(x));
        test_cond("sdscatrepr(...data...)",
                  memcmp(y, "\"\\a\\n\\x00foo\\r\"", 15) == 0);

        {
            unsigned int oldfree;
            char *p;
            int step = 10, j, i;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            test_cond("sdsnew() free/len buffers", sdslen(x) == 1 && sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                int oldlen = sdslen(x);
                x = sdsMakeRoomFor(x, step);
                int type = x[-1] & SDS_TYPE_MASK;

                test_cond("sdsMakeRoomFor() len", sdslen(x) == oldlen);
                if (type != SDS_TYPE_5) {
                    test_cond("sdsMakeRoomFor() free", sdsavail(x) >= step);
                    oldfree = sdsavail(x);
                }
                p = x + oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A' + j;
                }
                sdsIncrLen(x, step);
            }
            test_cond("sdsMakeRoomFor() content",
                      memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ", x, 101) == 0);
            test_cond("sdsMakeRoomFor() final length", sdslen(x) == 101);

            sdsfree(x);
        }
    }
    test_report();
    return 0;
}
#endif

#ifdef SDS_TEST_MAIN
int main(void) {
    return sdsTest();
}
#endif
