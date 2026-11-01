/* SPDX-License-Identifier: MIT */
/* Production APIs and real kernel locks; wrappers control scheduling only. */
ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(mutation_defaults, 0, 0, 0, 0);
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(mutation_array, "mutations", "array",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 4, 0, mutation_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
static const struct zmk_custom_setting_value mutation_churn_defaults[20] = {
    [0 ... 19] = ZMK_CUSTOM_SETTING_VALUE_INT32(0),
};
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(mutation_churn_array, "mutations", "churn",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 20, 0, mutation_churn_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
static void mutation_churn_views(struct k_work *work) {
    ARG_UNUSED(work);
    for (uint32_t i = 0; i < 20; i++) {
        CHECK(zmk_custom_setting_find_array_element("mutations", "churn", i) != NULL);
    }
}
K_WORK_DEFINE(mutation_churn_work, mutation_churn_views);
static void mutation_load_read_hook(const struct extra_record *record) {
    if (!operation ||
        (strcmp(operation, "mutation-load-view") && strcmp(operation, "mutation-load-size"))) {
        return;
    }
    const char *target = !strcmp(operation, "mutation-load-size")
                             ? "custom_settings/mutations/array/_size"
                             : "custom_settings/mutations/array/0";
    if (strcmp(record->name, target)) {
        return;
    }
    CHECK(k_work_submit(&mutation_churn_work) > 0);
    struct k_work_sync sync;
    k_work_flush(&mutation_churn_work, &sync);
}
extern struct k_mutex settings_lock;
K_SEM_DEFINE(mutation_paused, 0, 1);
K_SEM_DEFINE(mutation_resume, 0, 1);
K_THREAD_STACK_DEFINE(mutation_stack, 4096);
static struct k_thread mutation_thread;
static bool mutation_gate, mutation_intercepted, mutation_before_lock, mutation_check_events;
static const char *mutation_operation;
static int mutation_result = 99;
static struct zmk_custom_setting_value mutation_output;

static void mutation_pause(void) {
    if (mutation_gate && !mutation_intercepted && k_current_get() == &mutation_thread) {
        mutation_intercepted = true;
        k_sem_give(&mutation_paused);
        CHECK(k_sem_take(&mutation_resume, K_SECONDS(3)) == 0);
    }
}
void __real_custom_settings_state_lock(void);
void __real_custom_settings_state_unlock(void);
void __wrap_custom_settings_state_lock(void) {
    if (mutation_before_lock) {
        mutation_pause();
    }
    __real_custom_settings_state_lock();
}
void __wrap_custom_settings_state_unlock(void) {
    __real_custom_settings_state_unlock();
    if (!mutation_before_lock) {
        mutation_pause();
    }
}
static int mutation_event(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    if (mutation_check_events) {
        CHECK(custom_settings_lock.owner != k_current_get());
        CHECK(settings_lock.owner != k_current_get());
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(mutation_event_listener, mutation_event);
ZMK_SUBSCRIPTION(mutation_event_listener, zmk_custom_setting_changed);

static void mutation_push(int32_t n, enum zmk_custom_setting_write_mode mode) {
    struct zmk_custom_setting_value v = ZMK_CUSTOM_SETTING_VALUE_INT32(n);
    CHECK(zmk_custom_setting_array_push_back(&mutation_array, &v, mode) == 0);
}
static int32_t mutation_at(uint32_t i) {
    struct zmk_custom_setting_value v;
    CHECK(zmk_custom_setting_read_array_by_key("mutations", "array", i, &v) == 0);
    CHECK(v.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32);
    return v.int32_value;
}
static void mutation_worker(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    struct zmk_custom_setting_value v = ZMK_CUSTOM_SETTING_VALUE_INT32(111);
    if (!strcmp(mutation_operation, "push") || !strcmp(mutation_operation, "mixed")) {
        mutation_result = zmk_custom_setting_array_push_back(&mutation_array, &v,
                                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    } else if (!strcmp(mutation_operation, "pop")) {
        mutation_result = zmk_custom_setting_array_pop_back(&mutation_array, &mutation_output,
                                                            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    } else if (!strcmp(mutation_operation, "create")) {
        mutation_result = zmk_custom_setting_keyspace_create(
            &keys, "item/same", &ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    } else if (!strcmp(mutation_operation, "rename")) {
        mutation_result = zmk_custom_setting_keyspace_rename(&keys, "item/one", "item/new",
                                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    } else if (!strcmp(mutation_operation, "delete")) {
        mutation_result = zmk_custom_setting_keyspace_delete(&keys, "item/one");
    } else {
        CHECK(!strcmp(mutation_operation, "order"));
        const struct zmk_custom_setting *s = zmk_custom_setting_keyspace_find(&keys, "item/one");
        CHECK(s != NULL);
        mutation_result = zmk_custom_setting_write(s, &ZMK_CUSTOM_SETTING_VALUE_BYTES(3),
                                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    }
}
static void mutation_start(const char *name) {
    mutation_operation = name;
    mutation_gate = true;
    k_thread_create(&mutation_thread, mutation_stack, K_THREAD_STACK_SIZEOF(mutation_stack),
                    mutation_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    CHECK(k_sem_take(&mutation_paused, K_SECONDS(3)) == 0);
}
static void mutation_join(void) {
    k_sem_give(&mutation_resume);
    CHECK(k_thread_join(&mutation_thread, K_SECONDS(3)) == 0);
    CHECK(mutation_result == 0);
}
static void mutation_entry(const char *key, uint8_t payload) {
    CHECK(zmk_custom_setting_keyspace_create(&keys, key, &ZMK_CUSTOM_SETTING_VALUE_BYTES(payload),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL) == 0);
}
static void mutation_payload(const char *key, uint8_t expected) {
    const struct zmk_custom_setting *s = zmk_custom_setting_keyspace_find(&keys, key);
    struct zmk_custom_setting_value v;
    CHECK(s && zmk_custom_setting_read(s, &v) == 0);
    CHECK(v.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES && v.size == 1 &&
          v.bytes_value[0] == expected);
}
#include "notification_cases.inc"

static void mutation_cases(const char *name) {
    mutation_check_events = true;
    if (!strncmp(name, "notify-", 7)) {
        notification_cases(name);
    } else if (!strcmp(name, "control")) {
        mutation_push(111, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        mutation_push(222, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        struct zmk_custom_setting_value out;
        CHECK(zmk_custom_setting_array_pop_back(&mutation_array, &out,
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0 &&
              out.int32_value == 222);
        CHECK(zmk_custom_setting_array_pop_back(&mutation_array, &out,
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0 &&
              out.int32_value == 111);
        mutation_entry("item/one", 1);
        CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/one", "item/new",
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        mutation_payload("item/new", 1);
        CHECK(zmk_custom_setting_keyspace_delete(&keys, "item/new") == 0);
        CHECK(zmk_custom_setting_keyspace_find(&keys, "item/new") == NULL);
    } else if (!strcmp(name, "push") || !strcmp(name, "pop") || !strcmp(name, "mixed")) {
        if (!strcmp(name, "pop")) {
            mutation_push(111, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
            mutation_push(222, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        }
        if (!strcmp(name, "mixed")) {
            mutation_push(10, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        }
        mutation_start(name);
        if (!strcmp(name, "push")) {
            mutation_push(222, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        } else {
            struct zmk_custom_setting_value out;
            CHECK(zmk_custom_setting_array_pop_back(&mutation_array, &out,
                                                    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
            CHECK(out.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32);
            CHECK(out.int32_value == 111);
        }
        mutation_join();
        if (!strcmp(name, "push")) {
            CHECK(zmk_custom_setting_array_size(&mutation_array) == 2 && mutation_at(0) == 111 &&
                  mutation_at(1) == 222);
        } else if (!strcmp(name, "pop")) {
            CHECK(zmk_custom_setting_array_size(&mutation_array) == 0 &&
                  mutation_output.int32_value == 222);
        } else {
            CHECK(zmk_custom_setting_array_size(&mutation_array) == 1 && mutation_at(0) == 10);
        }
    } else if (!strcmp(name, "create")) {
        mutation_start(name);
        CHECK(zmk_custom_setting_keyspace_create(
                  &keys, "item/same", &ZMK_CUSTOM_SETTING_VALUE_BYTES(2),
                  ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL) == -EEXIST);
        mutation_join();
        mutation_payload("item/same", 1);
        CHECK(keys.slots[0].in_use && !keys.slots[1].in_use);
    } else if (!strcmp(name, "rename")) {
        mutation_entry("item/one", 1);
        mutation_entry("item/two", 2);
        mutation_start(name);
        CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/two", "item/new",
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == -EEXIST);
        mutation_join();
        mutation_payload("item/new", 1);
        mutation_payload("item/two", 2);
    } else if (!strcmp(name, "delete")) {
        mutation_entry("item/one", 1);
        mutation_start(name);
        int ret = zmk_custom_setting_keyspace_delete(&keys, "item/one");
        CHECK(ret == 0 || ret == -ENOENT);
        mutation_entry("item/new", 2);
        mutation_join();
        mutation_payload("item/new", 2);
    } else if (!strcmp(name, "order")) {
        mutation_entry("item/one", 1);
        mutation_entry("item/two", 2);
        mutation_check_events = false; /* The caller intentionally owns settings_lock. */
        CHECK(k_mutex_lock(&settings_lock, K_FOREVER) == 0);
        mutation_before_lock = true;
        mutation_start(name);
        const struct zmk_custom_setting *s = zmk_custom_setting_keyspace_find(&keys, "item/two");
        CHECK(s && zmk_custom_setting_write(s, &ZMK_CUSTOM_SETTING_VALUE_BYTES(4),
                                            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        k_mutex_unlock(&settings_lock);
        mutation_join();
        mutation_payload("item/one", 3);
        mutation_payload("item/two", 4);
    } else if (!strcmp(name, "invalid-create") || !strcmp(name, "invalid-rename")) {
        if (!strcmp(name, "invalid-create")) {
            CHECK(zmk_custom_setting_keyspace_create(
                      &keys, "item/temp", &ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                      ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY, NULL) == -EINVAL);
            CHECK(!keys.slots[0].in_use && !keys.slots[1].in_use);
        } else {
            mutation_entry("item/one", 1);
            CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/one", "item/temp",
                                                     ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) ==
                  -EINVAL);
            mutation_payload("item/one", 1);
            CHECK(zmk_custom_setting_keyspace_find(&keys, "item/temp") == NULL);
        }
    } else if (!strcmp(name, "load-view") || !strcmp(name, "load-size")) {
        uint32_t size = 1;
        int32_t stored = 77;
        strcpy(extra[0].name, "custom_settings/mutations/array/_size");
        extra[0].size = sizeof(size);
        memcpy(extra[0].data, &size, sizeof(size));
        extra[0].present = true;
        strcpy(extra[1].name, "custom_settings/mutations/array/0");
        extra[1].size = sizeof(stored);
        memcpy(extra[1].data, &stored, sizeof(stored));
        extra[1].present = true;
        CHECK(settings_load_subtree("custom_settings") == 0);
        CHECK(zmk_custom_setting_array_size(&mutation_array) == 1 && mutation_at(0) == 77);
        CHECK(zmk_custom_setting_array_size(&mutation_churn_array) == 0);
        for (uint32_t i = 0; i < 20; i++) {
            CHECK(mutation_churn_array.array_state->values[i].int32_value == 0);
        }
    } else if (!strcmp(name, "empty-save") || !strcmp(name, "empty-discard") ||
               !strcmp(name, "empty-reset") || !strcmp(name, "short-reset") ||
               !strcmp(name, "short-discard")) {
        mutation_push(111, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        mutation_push(222, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        CHECK(zmk_custom_setting_array_pop_back(&mutation_array, NULL,
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        bool empty = !strncmp(name, "empty-", 6);
        if (empty) {
            CHECK(zmk_custom_setting_array_pop_back(&mutation_array, NULL,
                                                    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        }
        uint32_t count = 0;
        bool discard = strstr(name, "discard") != NULL;
        bool save = !strcmp(name, "empty-save");
        int ret = save      ? zmk_custom_settings_save_scope("mutations", "array", NULL, &count)
                  : discard ? zmk_custom_settings_discard_scope("mutations", "array", NULL, &count)
                            : zmk_custom_settings_reset_scope("mutations", "array", NULL, &count);
        CHECK(ret == 0 && count == 1);
        CHECK(zmk_custom_setting_array_size(&mutation_array) == (discard ? 2 : 0));
        if (discard) {
            CHECK(mutation_at(0) == 111 && mutation_at(1) == 222);
        }
        CHECK(!zmk_custom_setting_has_unsaved_value(&mutation_array));
        CHECK(settings_load_subtree("custom_settings") == 0);
        CHECK(zmk_custom_setting_array_size(&mutation_array) == (discard ? 2 : 0));
        if (!save && !discard) {
            for (uint32_t i = 0; i < mutation_array.array_state->max_size; i++) {
                CHECK(!mutation_array.array_state->has_persistent[i]);
                CHECK(mutation_array.array_state->values[i].int32_value == 0);
            }
            for (unsigned i = 0; i < ARRAY_SIZE(extra); i++) {
                CHECK(!extra[i].present ||
                      strncmp(extra[i].name, "custom_settings/mutations/array/",
                              sizeof("custom_settings/mutations/array/") - 1));
            }
        }
        CHECK(value(first) == 20 && value(second) == 21);
        CHECK(zmk_custom_setting_array_size(&mutation_churn_array) == 0);
    } else if (!strcmp(name, "shift")) {
        mutation_push(10, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        mutation_push(20, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        mutation_push(30, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        CHECK(zmk_custom_setting_array_remove_at(&mutation_array, 1, NULL,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        const struct zmk_custom_setting *s =
            zmk_custom_setting_find_array_element("mutations", "array", 1);
        CHECK(s && zmk_custom_setting_has_unsaved_value(s));
        CHECK(zmk_custom_setting_discard(s) == 0 && mutation_at(1) == 20);
        CHECK(zmk_custom_setting_array_size(&mutation_array) == 3);
        CHECK(zmk_custom_setting_array_pop_back(&mutation_array, NULL,
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
        CHECK(zmk_custom_setting_array_insert_at(&mutation_array, 0,
                                                 &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        for (uint32_t i = 0; i < 3; i++) {
            s = zmk_custom_setting_find_array_element("mutations", "array", i);
            CHECK(s && zmk_custom_setting_has_unsaved_value(s));
        }
        for (uint32_t i = 0; i < 3; i++) {
            s = zmk_custom_setting_find_array_element("mutations", "array", i);
            CHECK(zmk_custom_setting_discard(s) == 0);
        }
        CHECK(zmk_custom_setting_array_size(&mutation_array) == 2 && mutation_at(0) == 10 &&
              mutation_at(1) == 20);
        CHECK(mutation_array.array_state->values[2].int32_value == 0);
    } else {
        mutation_push(10, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        mutation_push(20, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        struct zmk_custom_setting_value output = ZMK_CUSTOM_SETTING_VALUE_INT32(-1);
        const struct zmk_custom_setting *s =
            zmk_custom_setting_find_array_element("mutations", "array", 0);
        unsigned before = changes;
        int ret;
        if (!strcmp(name, "invalid-element")) {
            ret = zmk_custom_setting_write_array_element(s, &ZMK_CUSTOM_SETTING_VALUE_INT32(99), 1,
                                                         (enum zmk_custom_setting_write_mode)99);
        } else if (!strcmp(name, "invalid-insert")) {
            ret = zmk_custom_setting_array_insert_at(&mutation_array, 0,
                                                     &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                                     ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
        } else if (!strcmp(name, "invalid-pop")) {
            ret = zmk_custom_setting_array_pop_back(&mutation_array, &output,
                                                    (enum zmk_custom_setting_write_mode)99);
        } else {
            CHECK(!strcmp(name, "invalid-remove"));
            ret = zmk_custom_setting_array_remove_at(&mutation_array, 0, &output,
                                                     (enum zmk_custom_setting_write_mode)99);
        }
        CHECK(ret == -EINVAL);
        CHECK(zmk_custom_setting_array_size(&mutation_array) == 2 && mutation_at(0) == 10 &&
              mutation_at(1) == 20);
        CHECK(output.int32_value == -1 && changes == before);
    }
    CHECK(value(first) == 20 && value(second) == 21);
    printk("PASS mutation %s time=%lld\n", name, k_uptime_get());
}
