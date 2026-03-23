/* Unit tests for valkeyClusterGetSlotByKey. */

#include "cluster.h"
#include "test_utils.h"

#include <assert.h>

void test_slot_by_key_basic(void) {
    /* A simple key should produce a consistent slot. */
    unsigned int slot = valkeyClusterGetSlotByKey((char *)"foo", 3);
    assert(slot == valkeyClusterGetSlotByKey((char *)"foo", 3));
}

void test_slot_by_key_with_hashtag(void) {
    /* Keys with the same hash tag should map to the same slot. */
    unsigned int slot1 = valkeyClusterGetSlotByKey((char *)"{tag}key1", 9);
    unsigned int slot2 = valkeyClusterGetSlotByKey((char *)"{tag}key2", 9);
    assert(slot1 == slot2);
}

void test_slot_by_key_binary_safe(void) {
    /* Key with embedded null: "ke\0y" (length 4) should produce a
     * different slot than "ke" (length 2, what strlen would give). */
    unsigned int slot_full = valkeyClusterGetSlotByKey((char *)"ke\0y", 4);
    unsigned int slot_truncated = valkeyClusterGetSlotByKey((char *)"ke", 2);
    assert(slot_full != slot_truncated);
}

void test_slot_by_key_empty(void) {
    /* An empty string is a valid key. */
    unsigned int slot = valkeyClusterGetSlotByKey((char *)"", 0);
    assert(slot == 0);
}

int main(void) {
    test_slot_by_key_basic();
    test_slot_by_key_with_hashtag();
    test_slot_by_key_binary_safe();
    test_slot_by_key_empty();
    return 0;
}
