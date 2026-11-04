/* SPDX-License-Identifier: MIT */
/* Real registry/store/loader regressions; only the backend injects failures. */
ZMK_CUSTOM_SETTING_DEFINE_SIZED(capacity_bytes, 4, "blob_checks", "bytes",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_DEFINE_SIZED(capacity_string, 4, "blob_checks", "string",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                                ZMK_CUSTOM_SETTING_VALUE_STRING(""),
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
static const struct zmk_custom_setting_value bytes_options[] = {
    ZMK_CUSTOM_SETTING_VALUE_BYTES(0x11)};
static const struct zmk_custom_setting_value string_options[] = {
    ZMK_CUSTOM_SETTING_VALUE_STRING("yes")};
#define BLOB_OPTIONS(values_)                                                                      \
    {                                                                                              \
        .type = ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS, .options = {                                \
            .values = values_,                                                                     \
            .count = ARRAY_SIZE(values_)                                                           \
        }                                                                                          \
    }
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(constrained_bytes, "blob_checks", "bytes/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, 96, 16, 2,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   BLOB_OPTIONS(bytes_options));
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(constrained_string, "blob_checks", "string/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING, 96, 16, 2,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   BLOB_OPTIONS(string_options));
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE(load_keys, "blob_load", "item/",
                                                  ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, 8, 16, 2, 24,
                                                  ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                                  ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                                  ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                                  ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static void check_blob(const struct zmk_custom_setting *setting, const void *expected,
                       size_t size) {
    uint8_t actual[128];
    size_t read = 0;
    CHECK(zmk_custom_setting_read_into(setting, actual, sizeof(actual), &read, NULL) == 0);
    CHECK(read == size && !memcmp(actual, expected, size));
}
static void small_blob_capacity(void) {
    const struct zmk_custom_setting *settings[] = {&capacity_bytes, &capacity_string};
    for (size_t s = 0; s < ARRAY_SIZE(settings); s++) {
        const struct zmk_custom_setting *setting = settings[s];
        struct zmk_custom_setting_value exact =
            s ? ZMK_CUSTOM_SETTING_VALUE_STRING("abcd")
              : ZMK_CUSTOM_SETTING_VALUE_BYTES('a', 'b', 'c', 'd');
        struct zmk_custom_setting_value temporary =
            s ? ZMK_CUSTOM_SETTING_VALUE_STRING("xy") : ZMK_CUSTOM_SETTING_VALUE_BYTES('x', 'y');
        struct zmk_custom_setting_value invalid =
            s ? ZMK_CUSTOM_SETTING_VALUE_STRING("abcde")
              : ZMK_CUSTOM_SETTING_VALUE_BYTES('a', 'b', 'c', 'd', 'e');
        /* STRING storage follows actual length rather than a claimed short size. */
        if (s)
            invalid.size = 1;
        CHECK(zmk_custom_setting_write(setting, &exact, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) ==
              0);
        CHECK(!zmk_custom_setting_has_unsaved_value(setting));
        CHECK(zmk_custom_setting_write(setting, &temporary,
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) == 0);
        uint8_t state[sizeof(*setting->state)];
        memcpy(state, setting->state, sizeof(state));
        unsigned before = changes;
        CHECK(zmk_custom_setting_set_default(setting, &invalid) == -EMSGSIZE);
        CHECK(!memcmp(state, setting->state, sizeof(state)));
        for (int mode = ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;
             mode <= ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY; mode++) {
            CHECK(zmk_custom_setting_write(setting, &invalid, mode) == -EMSGSIZE);
            CHECK(zmk_custom_setting_write_quiet(setting, &invalid, mode) == -EMSGSIZE);
            CHECK(zmk_custom_setting_write_bytes(setting, "abcde", 5, mode) == -EMSGSIZE);
            CHECK(zmk_custom_setting_write_bytes_quiet(setting, "abcde", 5, mode) == -EMSGSIZE);
            CHECK(changes == before && !memcmp(state, setting->state, sizeof(state)));
            check_blob(setting, "xy", 2);
        }
        CHECK(zmk_custom_setting_rollback_temporary(setting) == 0);
        check_blob(setting, "abcd", 4);
        CHECK(zmk_custom_setting_discard(setting) == 0);
        check_blob(setting, "abcd", 4);
    }
    CHECK(value(first) == 20 && value(second) == 21);
    printk("PASS small blob capacity: BYTES/STRING typed/raw/quiet/all modes preserve "
           "memory/temp/durable state time=%lld\n",
           k_uptime_get());
}
static void raw_keyspace_constraints(void) {
    struct zmk_custom_setting_keyspace *spaces[] = {&constrained_bytes, &constrained_string};
    for (size_t s = 0; s < ARRAY_SIZE(spaces); s++) {
        const struct zmk_custom_setting_value *allowed = s ? string_options : bytes_options;
        struct zmk_custom_setting_value invalid =
            s ? ZMK_CUSTOM_SETTING_VALUE_STRING("no") : ZMK_CUSTOM_SETTING_VALUE_BYTES(0x22);
        const void *valid_data = s ? (const void *)"yes" : (const void *)allowed->bytes_value;
        const void *bad_data = s ? (const void *)"no" : (const void *)invalid.bytes_value;
        size_t size = s ? 3 : 1, bad_size = s ? 2 : 1;
        const struct zmk_custom_setting *entry = NULL;
        CHECK(zmk_custom_setting_keyspace_create(spaces[s], s ? "string/a" : "bytes/a", allowed,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST,
                                                 &entry) == 0);
        for (int mode = ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;
             mode <= ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY; mode++) {
            unsigned before = changes;
            uint8_t state[sizeof(*entry->state)];
            memcpy(state, entry->state, sizeof(state));
            CHECK(zmk_custom_setting_write(entry, &invalid, mode) == -EINVAL);
            CHECK(zmk_custom_setting_write_bytes(entry, bad_data, bad_size, mode) == -EINVAL);
            CHECK(zmk_custom_setting_write_bytes_quiet(entry, bad_data, bad_size, mode) == -EINVAL);
            CHECK(changes == before && !memcmp(state, entry->state, sizeof(state)));
            check_blob(entry, valid_data, size);
            CHECK(zmk_custom_setting_write_bytes_quiet(entry, valid_data, size, mode) == 0);
            CHECK(changes == before + 1);
            check_blob(entry, valid_data, size);
        }
        uint8_t large[80];
        memset(large, 'y', sizeof(large));
        unsigned before = changes;
        CHECK(zmk_custom_setting_write_bytes(entry, large, sizeof(large),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == -ENOTSUP);
        CHECK(zmk_custom_setting_write_bytes_quiet(
                  entry, large, sizeof(large), ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -ENOTSUP);
        CHECK(changes == before);
        check_blob(entry, valid_data, size);
        CHECK(zmk_custom_setting_rollback_temporary(entry) == 0);
    }
    /* Unconstrained large raw and its size ceiling remain supported. */
    uint8_t large[97];
    memset(large, 0x31, sizeof(large));
    const struct zmk_custom_setting *entry = NULL;
    struct zmk_custom_setting_value initial = ZMK_CUSTOM_SETTING_VALUE_BYTES(1);
    CHECK(zmk_custom_setting_keyspace_create(&keys, "item/large", &initial,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &entry) == 0);
    CHECK(zmk_custom_setting_write_bytes(entry, large, 80, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) ==
          0);
    check_blob(entry, large, 80);
    CHECK(zmk_custom_setting_write_bytes_quiet(entry, large, 97,
                                               ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == -EMSGSIZE);
    check_blob(entry, large, 80);
    CHECK(value(first) == 20 && value(second) == 21);
    printk("PASS raw keyspace constraints: BYTES/STRING typed-equivalent refusal, "
           "quiet/temporary/large controls time=%lld\n",
           k_uptime_get());
}
static void load_record(unsigned index, size_t size) {
    struct extra_record *record = &extra[index];
    memset(record, 0, sizeof(*record));
    snprintf(record->name, sizeof(record->name), "custom_settings/blob_load/item/#%u", index);
    memcpy(record->data, "item/old", 9);
    record->data[9] = 0x11;
    record->size = size;
    record->present = true;
}
static void rejected_keyspace_load(const char *kind) {
    bool live = !strncmp(kind, "live-", 5);
    if (live)
        kind += 5;
    struct zmk_custom_setting_value initial = ZMK_CUSTOM_SETTING_VALUE_BYTES(0x11);
    const struct zmk_custom_setting *old = NULL;
    if (live)
        CHECK(zmk_custom_setting_keyspace_create(&load_keys, "item/old", &initial,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &old) == 0);
    load_record(0, 10);
    if (!strcmp(kind, "oversize"))
        extra[0].size = 80;
    else if (!strcmp(kind, "scratch"))
        extra[0].size = 10000;
    else if (!strcmp(kind, "read"))
        read_error_name = extra[0].name;
    else if (!strcmp(kind, "short"))
        read_short_name = extra[0].name;
    else if (!strcmp(kind, "pool")) {
        memset(extra[0].data + 9, 0x11, 8);
        extra[0].size = 17;
        if (!live) {
            load_record(1, 17);
            memcpy(extra[1].data, "item/new", 9);
            memset(extra[1].data + 9, 0x22, 8);
        } else {
            const struct zmk_custom_setting *other = NULL;
            CHECK(zmk_custom_setting_keyspace_create(&load_keys, "item/hold", &initial,
                                                     ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
                                                     &other) == 0);
        }
    } else
        CHECK(false);
    unsigned before = changes;
    CHECK(settings_load_subtree("custom_settings/blob_load") == 0);
    CHECK(changes == before);
    CHECK(extra[0].reads == (!strcmp(kind, "scratch") ? 0 : 1));
    if (!strcmp(kind, "pool") && !live)
        CHECK(extra[1].reads == 1);
    read_error_name = read_short_name = NULL;
    if (live) {
        CHECK(load_keys.slots[0].in_use &&
              zmk_custom_setting_keyspace_find(&load_keys, "item/old") == old);
        check_blob(old, initial.bytes_value, 1);
        CHECK(zmk_custom_setting_has_unsaved_value(old));
        if (!strcmp(kind, "pool")) {
            const struct zmk_custom_setting *other =
                zmk_custom_setting_keyspace_find(&load_keys, "item/hold");
            CHECK(other == &load_keys.slots[1].setting);
            check_blob(other, initial.bytes_value, 1);
        }
    } else {
        unsigned rejected = !strcmp(kind, "pool") ? 1 : 0;
        CHECK(!load_keys.slots[rejected].in_use);
        extra[0].present = extra[1].present = false;
        if (rejected)
            CHECK(zmk_custom_setting_keyspace_delete(&load_keys, "item/old") == 0);
        CHECK(zmk_custom_setting_large_pool_used(load_keys.large_pool) == 0);
        const struct zmk_custom_setting *entry = NULL;
        CHECK(zmk_custom_setting_keyspace_create(&load_keys, "item/reuse", &initial,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
                                                 &entry) == 0);
        CHECK(entry == &load_keys.slots[0].setting);
        check_blob(entry, initial.bytes_value, 1);
    }
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    printk("PASS rejected keyspace load kind=%s live=%d keeps ownership and reusable slots "
           "time=%lld\n",
           kind, live, k_uptime_get());
}

ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(capacity_pool, 8);
ZMK_CUSTOM_SETTING_DEFINE_POOLED(pool_first, 8, capacity_pool, "blob_checks", "pool_first",
                                 ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                 ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
                                 ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_DEFINE_POOLED(pool_second, 8, capacity_pool, "blob_checks", "pool_second",
                                 ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                 ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
                                 ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
static void small_blob_pool_failure(void) {
    static const struct zmk_custom_setting_value oversized_default =
        ZMK_CUSTOM_SETTING_VALUE_BYTES('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h');
    uint8_t initial_state[sizeof(*pool_first.state)];
    memcpy(initial_state, pool_first.state, sizeof(initial_state));
    CHECK(zmk_custom_setting_set_default(&pool_first, &oversized_default) == -ENOSPC);
    CHECK(!memcmp(initial_state, pool_first.state, sizeof(initial_state)));
    CHECK(zmk_custom_setting_write_bytes(&pool_first, "abcd", 4,
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
    CHECK(zmk_custom_setting_write_bytes(&pool_second, "wxyz", 4,
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
    struct zmk_custom_setting_value temporary = ZMK_CUSTOM_SETTING_VALUE_BYTES('t');
    CHECK(zmk_custom_setting_write(&pool_first, &temporary,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) == 0);
    uint8_t state[sizeof(*pool_first.state)];
    memcpy(state, pool_first.state, sizeof(state));
    unsigned before = changes;
    CHECK(zmk_custom_setting_write_bytes(&pool_first, "abcdefgh", 8,
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == -ENOSPC);
    CHECK(zmk_custom_setting_write_bytes_quiet(&pool_first, "abcdefgh", 8,
                                               ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -ENOSPC);
    CHECK(changes == before && !memcmp(state, pool_first.state, sizeof(state)));
    check_blob(&pool_first, "t", 1);
    check_blob(&pool_second, "wxyz", 4);
    CHECK(zmk_custom_setting_rollback_temporary(&pool_first) == 0);
    check_blob(&pool_first, "abcd", 4);
    printk("PASS pool refusal retains value/temp/independent member and ENOSPC time=%lld\n",
           k_uptime_get());
}
static void valid_keyspace_load(void) {
    load_record(0, 10);
    CHECK(settings_load_subtree("custom_settings/blob_load") == 0);
    const struct zmk_custom_setting *entry =
        zmk_custom_setting_keyspace_find(&load_keys, "item/old");
    CHECK(entry == &load_keys.slots[0].setting);
    uint8_t expected = 0x11;
    check_blob(entry, &expected, 1);
    CHECK(!zmk_custom_setting_has_unsaved_value(entry));
    struct zmk_custom_setting_value next = ZMK_CUSTOM_SETTING_VALUE_BYTES(0x22);
    const struct zmk_custom_setting *other = NULL;
    CHECK(zmk_custom_setting_keyspace_create(&load_keys, "item/new", &next,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &other) == 0);
    CHECK(other == &load_keys.slots[1].setting);
    CHECK(zmk_custom_setting_keyspace_delete(&load_keys, "item/old") == 0);
    CHECK(zmk_custom_setting_keyspace_create(&load_keys, "item/reuse", &next,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &entry) == 0);
    CHECK(entry == &load_keys.slots[0].setting);
    check_blob(other, next.bytes_value, 1);
    check_blob(entry, next.bytes_value, 1);
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    printk(
        "PASS valid load retains payload and live identity, then delete/reuse recovers time=%lld\n",
        k_uptime_get());
}
