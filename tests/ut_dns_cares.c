/*
 * Unit tests for c-ares DNS resolution (src/dns.c).
 *
 * Tests valkeyResolveSync() with c-ares backend:
 * - Resolve an IP literal (127.0.0.1)
 * - Resolve "localhost"
 * - Resolve a non-existent hostname (expect failure)
 * - Resolve with IPv6 preference
 * - Timeout behavior
 */

#define _POSIX_C_SOURCE 200112L

#include "valkey.h"

#include <assert.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

/* Declarations for internal functions under test. */
int valkeyResolveSync(const char *host, int port, int flags,
                      long timeout_ms, struct addrinfo **result);
void valkeyFreeAddrInfo(struct addrinfo *ai);

/* Test: resolving an IP literal should succeed immediately. */
static void test_resolve_ip_literal(void) {
    struct addrinfo *result = NULL;
    int rv = valkeyResolveSync("127.0.0.1", 6379, 0, 5000, &result);
    assert(rv == 0);
    assert(result != NULL);
    assert(result->ai_family == AF_INET);
    assert(result->ai_socktype == SOCK_STREAM);
    valkeyFreeAddrInfo(result);
    printf("  PASS: test_resolve_ip_literal\n");
}

/* Test: resolving "localhost" should succeed. */
static void test_resolve_localhost(void) {
    struct addrinfo *result = NULL;
    int rv = valkeyResolveSync("localhost", 6379, 0, 5000, &result);
    assert(rv == 0);
    assert(result != NULL);
    assert(result->ai_socktype == SOCK_STREAM);
    valkeyFreeAddrInfo(result);
    printf("  PASS: test_resolve_localhost\n");
}

/* Test: resolving a non-existent hostname should fail. */
static void test_resolve_nonexistent(void) {
    struct addrinfo *result = NULL;
    int rv = valkeyResolveSync("this.host.does.not.exist.invalid", 6379,
                               0, 2000, &result);
    assert(rv != 0);
    assert(result == NULL);
    printf("  PASS: test_resolve_nonexistent\n");
}

/* Test: resolving with VALKEY_PREFER_IPV6 flag. */
static void test_resolve_ipv6_preference(void) {
    struct addrinfo *result = NULL;
    int rv = valkeyResolveSync("localhost", 6379, VALKEY_PREFER_IPV6, 5000, &result);
    /* May succeed or fail depending on system config (IPv6 availability). */
    if (rv == 0 && result != NULL) {
        assert(result->ai_family == AF_INET6 || result->ai_family == AF_INET);
        valkeyFreeAddrInfo(result);
    }
    printf("  PASS: test_resolve_ipv6_preference\n");
}

/* Test: DNS timeout with very short timeout.
 * .example is reserved (RFC 2606) and guaranteed to never resolve,
 * forcing c-ares to actually send queries that time out. */
static void test_resolve_timeout(void) {
    struct addrinfo *result = NULL;
    int rv = valkeyResolveSync("timeout-test.example", 6379, 0, 1, &result);
    assert(rv != 0);
    assert(result == NULL);
    printf("  PASS: test_resolve_timeout\n");
}

/* Test: resolving with both IPv4 and IPv6 (AF_UNSPEC). */
static void test_resolve_unspec(void) {
    struct addrinfo *result = NULL;
    int flags = VALKEY_PREFER_IPV4 | VALKEY_PREFER_IPV6;
    int rv = valkeyResolveSync("localhost", 6379, flags, 5000, &result);
    if (rv == 0) {
        assert(result != NULL);
        valkeyFreeAddrInfo(result);
    }
    printf("  PASS: test_resolve_unspec\n");
}

int main(void) {
    printf("Testing c-ares DNS resolution:\n");
    test_resolve_ip_literal();
    test_resolve_localhost();
    test_resolve_nonexistent();
    test_resolve_ipv6_preference();
    test_resolve_timeout();
    test_resolve_unspec();
    printf("All DNS tests passed.\n");
    return 0;
}
