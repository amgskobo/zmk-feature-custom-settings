/* SPDX-License-Identifier: MIT */
/* Review regressions use the production loader and typed registry. */
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(review_int_keys, "review", "typed/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 0, 16, 2,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(review_bool_keys, "review", "flag/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, 0, 16, 2,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static void review_record(unsigned record, const char *subsystem, const char *prefix,
                          unsigned ordinal, const char *key, const void *payload, size_t size) {
    struct extra_record *entry = &extra[record];
    *entry = (struct extra_record){0};
    snprintf(entry->name, sizeof(entry->name), "custom_settings/%s/%s#%u", subsystem, prefix,
             ordinal);
    size_t key_size = strlen(key) + 1;
    CHECK(key_size + size <= sizeof(entry->data));
    memcpy(entry->data, key, key_size);
    memcpy(entry->data + key_size, payload, size);
    entry->size = key_size + size;
    entry->present = true;
}

static void review_ordinal_names(bool reverse) {
    uint8_t first_payload[] = {0x31, 0x32}, second_payload[] = {0x42, 0x43};
    review_record(reverse ? 1 : 0, "boundary", "item/", 0, "item/#1", first_payload,
                  sizeof(first_payload));
    review_record(reverse ? 0 : 1, "boundary", "item/", 1, "item/#0", second_payload,
                  sizeof(second_payload));
    CHECK(settings_load_subtree("custom_settings/boundary") == 0);
    CHECK(zmk_custom_setting_keyspace_find(&keys, "item/#1") == &keys.slots[0].setting);
    CHECK(zmk_custom_setting_keyspace_find(&keys, "item/#0") == &keys.slots[1].setting);
    ordinal_payload(&keys.slots[0].setting, "item/#1", first_payload[0]);
    ordinal_payload(&keys.slots[1].setting, "item/#0", second_payload[0]);
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    printk("PASS ordinal-like public names retain distinct storage slots order=%u\n", reverse);
}

static void review_invalid_storage(const char *kind) {
    const uint8_t byte = 0x11;
    const uint8_t valid_payload[] = {0x11, 0x12};
    struct zmk_custom_setting_keyspace *space = &keys;
    unsigned rejected = 0;
    review_record(0, "boundary", "item/", 0, "item/valid", valid_payload, sizeof(valid_payload));
    if (!strcmp(kind, "missing")) {
        memset(extra[0].data, 'x', 8);
        extra[0].size = 8;
    } else if (!strcmp(kind, "prefix")) {
        review_record(0, "boundary", "item/", 0, "other/key", &byte, 1);
    } else if (!strcmp(kind, "long")) {
        review_record(0, "boundary", "item/", 0, "item/abcdefghijkl", &byte, 1);
    } else if (!strcmp(kind, "payload")) {
        uint8_t too_large[97];
        memset(too_large, 0x55, sizeof(too_large));
        review_record(0, "boundary", "item/", 0, "item/valid", too_large, sizeof(too_large));
    } else if (!strcmp(kind, "duplicate")) {
        review_record(1, "boundary", "item/", 1, "item/valid", valid_payload,
                      sizeof(valid_payload));
        rejected = 1;
    } else if (!strcmp(kind, "type")) {
        const uint8_t short_int[] = {1, 2, 3};
        review_record(0, "review", "typed/", 0, "typed/value", short_int, sizeof(short_int));
        space = &review_int_keys;
    } else {
        CHECK(!strcmp(kind, "constraint"));
        const uint8_t refused = 0x22;
        review_record(0, "blob_checks", "bytes/", 0, "bytes/value", &refused, 1);
        space = &constrained_bytes;
    }
    unsigned before = changes;
    CHECK(settings_load_subtree("custom_settings") == 0);
    CHECK(extra[rejected].reads == 1 && !space->slots[rejected].in_use);
    CHECK(changes == before);
    if (rejected) {
        CHECK(space->slots[0].in_use);
        ordinal_payload(&space->slots[0].setting, "item/valid", byte);
    }
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    printk("PASS malformed stored keyspace record refused kind=%s\n", kind);
}

static void review_public_key_copy(void) {
    const struct zmk_custom_setting *alpha = NULL, *beta = NULL;
    struct zmk_custom_setting_value payload = ZMK_CUSTOM_SETTING_VALUE_BYTES(3);
    CHECK(zmk_custom_setting_keyspace_create(&keys, "item/alpha", &payload,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &alpha) == 0);
    CHECK(zmk_custom_setting_keyspace_create(&keys, "item/beta", &payload,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &beta) == 0);
    char alpha_key[16], beta_key[16], small[3];
    CHECK(zmk_custom_setting_copy_public_key(alpha, alpha_key, sizeof(alpha_key)) == 0);
    CHECK(zmk_custom_setting_copy_public_key(beta, beta_key, sizeof(beta_key)) == 0);
    CHECK(!strcmp(alpha_key, "item/alpha") && !strcmp(beta_key, "item/beta"));
    CHECK(zmk_custom_setting_copy_public_key(alpha, small, sizeof(small)) == -ENAMETOOLONG);
    CHECK(small[0] == '\0' && !strcmp(alpha_key, "item/alpha"));
    /* A retained pre-validation blob must never be truncated through the
     * legacy shared scratch buffer before the caller's bound is checked. */
    uint8_t retained[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN + 20];
    memset(retained, 'x', sizeof(retained));
    uint8_t *saved_data = alpha->state->blob.data;
    size_t saved_size = alpha->state->blob.size;
    alpha->state->blob.data = retained;
    alpha->state->blob.size = sizeof(retained);
    CHECK(zmk_custom_setting_copy_public_key(alpha, alpha_key, sizeof(alpha_key)) == -EINVAL);
    CHECK(alpha_key[0] == '\0');
    retained[sizeof(retained) - 1] = '\0';
    char complete[sizeof(retained)];
    CHECK(zmk_custom_setting_copy_public_key(alpha, complete, sizeof(complete)) == 0);
    CHECK(strlen(complete) == sizeof(retained) - 1 &&
          !memcmp(complete, retained, sizeof(retained)));
    CHECK(zmk_custom_setting_copy_public_key(alpha, alpha_key, sizeof(alpha_key)) == -ENAMETOOLONG);
    CHECK(alpha_key[0] == '\0');
    alpha->state->blob.data = saved_data;
    alpha->state->blob.size = saved_size;
    CHECK(zmk_custom_setting_keyspace_delete(&keys, "item/alpha") == 0);
    CHECK(zmk_custom_setting_copy_public_key(alpha, alpha_key, sizeof(alpha_key)) == -ENOENT);
    CHECK(!strcmp(beta_key, "item/beta"));
    printk("PASS public key copies survive independent lookups; bounds and deletion refused\n");
}

static void review_scalar_storage(void) {
    const int32_t number = 123456;
    const uint8_t enabled = 1, invalid = 2;
    review_record(0, "review", "typed/", 0, "typed/value", &number, sizeof(number));
    review_record(1, "review", "flag/", 0, "flag/valid", &enabled, sizeof(enabled));
    review_record(2, "review", "flag/", 1, "flag/bad", &invalid, sizeof(invalid));
    CHECK(settings_load_subtree("custom_settings/review") == 0);
    const struct zmk_custom_setting *integer =
        zmk_custom_setting_keyspace_find(&review_int_keys, "typed/value");
    const struct zmk_custom_setting *boolean =
        zmk_custom_setting_keyspace_find(&review_bool_keys, "flag/valid");
    struct zmk_custom_setting_value loaded;
    CHECK(integer && boolean);
    CHECK(zmk_custom_setting_read(integer, &loaded) == 0 &&
          loaded.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 && loaded.int32_value == number);
    CHECK(zmk_custom_setting_read(boolean, &loaded) == 0 &&
          loaded.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL && loaded.bool_value);
    CHECK(!review_bool_keys.slots[1].in_use && extra[2].reads == 1);
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    printk("PASS scalar keyspaces ignore blob-only max_size while rejecting invalid booleans\n");
}

static void review_atomic_array_growth(void) {
    struct zmk_custom_setting_value next = ZMK_CUSTOM_SETTING_VALUE_INT32(77), actual;
    CHECK(zmk_custom_setting_write_array_element_by_key("mutations", "churn", 4, &next, 0,
                                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
    CHECK(zmk_custom_setting_array_size(&mutation_churn_array) == 5);
    CHECK(zmk_custom_setting_read_array_by_key("mutations", "churn", 4, &actual) == 0 &&
          actual.int32_value == 77);
    next.int32_value = 88;
    CHECK(zmk_custom_setting_write_array_element_by_key("mutations", "churn", 1, &next, 0,
                                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
    CHECK(zmk_custom_setting_array_size(&mutation_churn_array) == 5);
    CHECK(zmk_custom_setting_read_array_by_key("mutations", "churn", 4, &actual) == 0 &&
          actual.int32_value == 77);
    CHECK(zmk_custom_setting_write_array_element_by_key("mutations", "churn", UINT32_MAX, &next, 0,
                                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) < 0);
    CHECK(zmk_custom_setting_array_size(&mutation_churn_array) == 5);
    CHECK(value(first) == 20 && value(second) == 21);
    printk("PASS atomic by-key array writes grow without shrinking later elements\n");
}

static const char *review_scope_operation;
static uint32_t review_scope_affected;
static bool review_scope_gate;
int __real_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);
int __wrap_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout) {
    /* Calls between functions in custom_settings.c are resolved within that
     * object, so --wrap=custom_settings_state_lock cannot intercept them.
     * Pause at the real paired guard's first kernel lock instead. */
    if (review_scope_gate && mutex == &settings_lock) {
        mutation_pause();
    }
    return __real_z_impl_k_mutex_lock(mutex, timeout);
}
static void review_scope_worker(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    if (!strcmp(review_scope_operation, "save")) {
        mutation_result =
            zmk_custom_settings_save_scope("boundary", "item/old", NULL, &review_scope_affected);
    } else if (!strcmp(review_scope_operation, "discard")) {
        mutation_result =
            zmk_custom_settings_discard_scope("boundary", "item/old", NULL, &review_scope_affected);
    } else {
        mutation_result =
            zmk_custom_settings_reset_scope("boundary", "item/old", NULL, &review_scope_affected);
    }
}

static void review_scope_identity(const char *kind) {
    mutation_entry("item/old", 1);
    const struct zmk_custom_setting *old = zmk_custom_setting_keyspace_find(&keys, "item/old");
    review_scope_operation = kind;
    review_scope_gate = true;
    mutation_gate = true;
    mutation_before_lock = true;
    mutation_intercepted = false;
    mutation_check_events = true;
    k_thread_create(&mutation_thread, mutation_stack, K_THREAD_STACK_SIZEOF(mutation_stack),
                    review_scope_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    CHECK(k_sem_take(&mutation_paused, K_SECONDS(3)) == 0);
    CHECK(zmk_custom_setting_keyspace_delete(&keys, "item/old") == 0);
    mutation_entry("item/new", 2);
    const struct zmk_custom_setting *replacement =
        zmk_custom_setting_keyspace_find(&keys, "item/new");
    CHECK(old == replacement);
    CHECK(zmk_custom_setting_has_unsaved_value(replacement));
    mutation_join();
    CHECK(review_scope_affected == 0);
    mutation_payload("item/new", 2);
    CHECK(zmk_custom_setting_has_unsaved_value(replacement));
    mutation_gate = false;
    review_scope_gate = false;
    uint32_t affected = 0;
    // Serial matching-target control still performs the requested operation.
    CHECK(zmk_custom_settings_save_scope("boundary", "item/new", NULL, &affected) == 0 &&
          affected == 1 && !zmk_custom_setting_has_unsaved_value(replacement));
    CHECK(value(first) == 20 && value(second) == 21);
    printk("PASS scope selection cannot mutate a reused keyspace slot operation=%s\n", kind);
}

static void review_storage_cases(const char *kind) {
    if (!strcmp(kind, "ordinal-forward") || !strcmp(kind, "ordinal-reverse")) {
        review_ordinal_names(!strcmp(kind, "ordinal-reverse"));
    } else if (!strcmp(kind, "copy")) {
        review_public_key_copy();
    } else if (!strcmp(kind, "grow")) {
        review_atomic_array_growth();
    } else if (!strcmp(kind, "scalars")) {
        review_scalar_storage();
    } else if (!strncmp(kind, "scope-", 6)) {
        review_scope_identity(kind + 6);
    } else {
        CHECK(!strncmp(kind, "load-", 5));
        review_invalid_storage(kind + 5);
    }
}
