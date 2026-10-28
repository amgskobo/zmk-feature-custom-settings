/* SPDX-License-Identifier: MIT */
/* Complete production core, real settings/mutex/thread and ZMK event manager.
 * Only the storage backend and interleaving gates are controlled. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <cormoran/zmk/custom_settings.h>

LOG_MODULE_REGISTER(zmk, 0);
extern struct k_mutex custom_settings_lock;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printk("HARNESS FAIL line=%d time=%lld: %s\n", __LINE__, k_uptime_get(), #expr);       \
            exit(3);                                                                               \
        }                                                                                          \
    } while (0)

ZMK_CUSTOM_SETTING_DEFINE(first_setting, "boundary", "scalar_first",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(10),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_DEFINE(second_setting, "boundary", "scalar_second",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(11),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static const struct zmk_custom_setting *first = &first_setting;
static const struct zmk_custom_setting *second = &second_setting;

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(array_defaults, 1, 2);
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(array_setting, "boundary", "array",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 2, 2, array_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_DEFINE_SIZED(blob_setting, 96, "boundary", "blob",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(keys, "boundary", "item/", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                   96, 16, 2, ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
/* Two short keys fit; growing one key exceeds this shared budget. */
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE(rename_keys, "boundary", "rename/",
                                                  ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, 4, 16, 2, 24,
                                                  ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                                  ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                                  ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                                  ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif

static struct extra_record {
    char name[96];
    uint8_t data[192];
    size_t size;
    bool present;
    unsigned reads;
} extra[8];
static bool fail_extra;

K_SEM_DEFINE(load_entered, 0, 1);
K_SEM_DEFINE(allow_load, 0, 1);
K_SEM_DEFINE(writer_at_api, 0, 1);
K_THREAD_STACK_DEFINE(load_stack, 4096);
K_THREAD_STACK_DEFINE(writer_stack, 4096);
static struct k_thread loader, writer;
static bool pause_load, monitor_writer, writer_holds_state, present[2] = {true, true};
static int32_t disk[2] = {20, 21};
static int fail_save = -1, read_error, load_result = 99, writer_result = 99;
/* Fails only the extra record with this storage name. */
static const char *read_error_name;
static unsigned saved[2], discarded, initialized, changes;
static const char *operation;

/* A settings loader may wait for a synchronous handoff to sysworkq while
 * holding settings_lock (e.g. ZMK's advertising commit). Queue the value
 * consumer first to exercise that ordering with the real kernel locks. */
static bool queue_cycle;
static int queue_read_result = 99;
K_SEM_DEFINE(queue_read_entered, 0, 1);

static void queue_read(struct k_work *work) {
    ARG_UNUSED(work);
    CHECK(k_current_get() == &k_sys_work_q.thread);
    k_sem_give(&queue_read_entered);
    struct zmk_custom_setting_value readback;
    if (!strcmp(operation, "queue-scalar") || !strcmp(operation, "memory-guard")) {
        const struct zmk_custom_setting *setting =
            zmk_custom_setting_find("boundary", "scalar_first");
        CHECK(setting == first);
        CHECK(zmk_custom_setting_read_by_key("boundary", "scalar_first", &readback) == 0);
        CHECK(readback.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 &&
              readback.int32_value == (!strcmp(operation, "memory-guard") ? 20 : 40));
        CHECK(zmk_custom_setting_read_default(setting, &readback) == 0 &&
              readback.int32_value == 10);
        CHECK(!zmk_custom_setting_matches_default(setting) &&
              !zmk_custom_setting_has_unsaved_value(setting));
        int32_t raw = 0;
        size_t size = 0;
        enum zmk_custom_setting_value_type type;
        CHECK(zmk_custom_setting_read_into(setting, &raw, sizeof(raw), &size, &type) == 0);
        CHECK(raw == (!strcmp(operation, "memory-guard") ? 20 : 40) && size == sizeof(raw) &&
              type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32);
        CHECK(zmk_custom_setting_read_into(setting, &raw, sizeof(raw) - 1, NULL, NULL) ==
              -EMSGSIZE);
        CHECK(zmk_custom_setting_read_by_key("boundary", "absent", &readback) == -ENOENT);
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
    } else if (!strcmp(operation, "queue-array")) {
        const struct zmk_custom_setting *view =
            zmk_custom_setting_find_array_element("boundary", "array", 0);
        CHECK(view != NULL && zmk_custom_setting_array_size(&array_setting) == 2);
        CHECK(zmk_custom_setting_read_array_by_key("boundary", "array", 0, &readback) == 0 &&
              readback.int32_value == 4);
        /* Index views have no scalar default descriptor; array defaults are
         * managed by the array API. */
        CHECK(zmk_custom_setting_read_default(view, &readback) == -ENOENT);
        CHECK(!zmk_custom_setting_has_unsaved_value(view));
        CHECK(zmk_custom_setting_find_array_element("boundary", "array", 2) == NULL);
        CHECK(zmk_custom_setting_read_array_by_key("boundary", "array", 2, &readback) == -ENOENT);
    } else if (!strcmp(operation, "queue-blob") || !strcmp(operation, "queue-keyspace")) {
        bool keyspace = !strcmp(operation, "queue-keyspace");
        const struct zmk_custom_setting *setting = &blob_setting;
        if (keyspace) {
            CHECK(zmk_custom_settings_keyspace_find_for_key("boundary", "item/test") == &keys);
            CHECK(zmk_custom_setting_keyspace_find(&keys, "item/missing") == NULL);
            setting = zmk_custom_setting_find("boundary", "item/test");
            CHECK(setting != NULL && !strcmp(zmk_custom_setting_public_key(setting), "item/test"));
        }
        size_t expected = keyspace ? 2 : 96, size = 0;
        CHECK(zmk_custom_setting_value_size(setting, &size) == 0 && size == expected);
        uint8_t raw[96];
        enum zmk_custom_setting_value_type type;
        CHECK(zmk_custom_setting_read_into(setting, raw, sizeof(raw), &size, &type) == 0 &&
              size == expected && type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES);
        for (size_t i = 0; i < size; i++) {
            CHECK(raw[i] == 0x31);
        }
        CHECK(zmk_custom_setting_read_into(setting, raw, expected - 1, NULL, NULL) == -EMSGSIZE);
        CHECK(!zmk_custom_setting_has_unsaved_value(setting));
        int ret = zmk_custom_setting_read(setting, &readback);
        if (keyspace) {
            CHECK(ret == 0 && readback.size == 2 && readback.bytes_value[0] == 0x31 &&
                  readback.bytes_value[1] == 0x31);
        } else {
            CHECK(ret == -EMSGSIZE);
        }
#endif
    } else {
        CHECK(false);
    }
    queue_read_result = 0;
}
static void handoff(struct k_work *work) { ARG_UNUSED(work); }
K_WORK_DEFINE(queue_read_work, queue_read);
K_WORK_DEFINE(handoff_work, handoff);

static int observe(const zmk_event_t *event) {
    const struct zmk_custom_setting_changed *change = as_zmk_custom_setting_changed(event);
    if (change) {
        changes++;
    }
    if (change && change->kind == ZMK_CUSTOM_SETTING_CHANGED_DISCARDED) {
        discarded++;
    }
    if (as_zmk_custom_settings_initialized(event)) {
        initialized++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(boundary, observe);
ZMK_SUBSCRIPTION(boundary, zmk_custom_setting_changed);
ZMK_SUBSCRIPTION(boundary, zmk_custom_settings_initialized);

int __real_settings_save_one(const char *, const void *, size_t);
int __real_settings_delete(const char *);
int __real_settings_load_subtree(const char *);
static void at_api(void) {
    if (monitor_writer && k_current_get() == &writer) {
        writer_holds_state = custom_settings_lock.owner == k_current_get();
        k_sem_give(&writer_at_api);
    }
}
int __wrap_settings_save_one(const char *name, const void *data, size_t size) {
    at_api();
    return __real_settings_save_one(name, data, size);
}
int __wrap_settings_delete(const char *name) {
    at_api();
    return __real_settings_delete(name);
}
int __wrap_settings_load_subtree(const char *name) {
    at_api();
    return __real_settings_load_subtree(name);
}

static ssize_t read_value(void *context, void *data, size_t size) {
    if (read_error) {
        return read_error;
    }
    CHECK(size == sizeof(int32_t));
    memcpy(data, context, size);
    return size;
}
static ssize_t read_extra(void *context, void *data, size_t size) {
    struct extra_record *record = context;
    record->reads++;
    if (read_error) {
        return read_error;
    }
    if (read_error_name && !strcmp(record->name, read_error_name)) {
        return -EIO;
    }
    CHECK(size <= record->size);
    memcpy(data, record->data, size);
    return size;
}
static const char *names[2] = {"custom_settings/boundary/scalar_first",
                               "custom_settings/boundary/scalar_second"};
static int backend_load(struct settings_store *store, const struct settings_load_arg *arg) {
    ARG_UNUSED(store);
    if (pause_load) {
        pause_load = false;
        k_sem_give(&load_entered);
        CHECK(k_sem_take(&allow_load, K_SECONDS(1)) == 0);
    }
    for (unsigned i = 0; i < 2; i++) {
        if (present[i]) {
            int ret =
                settings_call_set_handler(names[i], sizeof(disk[i]), read_value, &disk[i], arg);
            if (ret < 0) {
                return ret;
            }
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(extra); i++) {
        if (extra[i].present) {
            int ret =
                settings_call_set_handler(extra[i].name, extra[i].size, read_extra, &extra[i], arg);
            if (ret < 0) {
                return ret;
            }
        }
    }
    if (queue_cycle) {
        queue_cycle = false;
        CHECK(k_work_submit(&queue_read_work) > 0);
        CHECK(k_work_submit(&handoff_work) > 0);
        struct k_work_sync sync;
        /* false also means success when the handoff already completed. */
        k_work_flush(&handoff_work, &sync);
    }
    return 0;
}
static int backend_save(struct settings_store *store, const char *name, const char *data,
                        size_t size) {
    ARG_UNUSED(store);
    if (strcmp(name, names[0]) && strcmp(name, names[1])) {
        if (fail_extra) {
            return -EIO;
        }
        struct extra_record *record = NULL;
        for (unsigned i = 0; i < ARRAY_SIZE(extra); i++) {
            if (!strcmp(name, extra[i].name)) {
                record = &extra[i];
                break;
            }
            if (!record && !extra[i].name[0]) {
                record = &extra[i];
            }
        }
        CHECK(record && strlen(name) < sizeof(record->name) && size <= sizeof(record->data));
        strcpy(record->name, name);
        record->size = size;
        record->present = data != NULL;
        if (data) {
            memcpy(record->data, data, size);
        }
        return 0;
    }
    unsigned i = strcmp(name, names[0]) == 0 ? 0 : 1;
    CHECK(strcmp(name, names[i]) == 0);
    saved[i]++;
    if ((int)i == fail_save) {
        return -EIO;
    }
    present[i] = data != NULL;
    if (data) {
        CHECK(size == sizeof(disk[i]));
        memcpy(&disk[i], data, size);
    }
    return 0;
}
static const struct settings_store_itf backend_itf = {
    .csi_load = backend_load,
    .csi_save = backend_save,
};
static struct settings_store backend = {.cs_itf = &backend_itf};

static int32_t value(const struct zmk_custom_setting *setting) {
    int32_t result;
    CHECK(zmk_custom_setting_get_int32(setting, &result) == 0);
    return result;
}
static void memory(const struct zmk_custom_setting *setting, int32_t number) {
    CHECK(zmk_custom_setting_set_int32(setting, number, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
}
static void load_thread(void *a, void *b, void *c) {
    load_result = settings_load_subtree("custom_settings");
}
static void writer_thread(void *a, void *b, void *c) {
    if (strcmp(operation, "save") == 0) {
        writer_result = zmk_custom_setting_save(first);
    } else if (strcmp(operation, "discard") == 0) {
        writer_result = zmk_custom_setting_discard(first);
    } else {
        writer_result = zmk_custom_setting_reset(first);
    }
}

static void race(void) {
    memory(first, 30);
    pause_load = true;
    k_thread_create(&loader, load_stack, K_THREAD_STACK_SIZEOF(load_stack), load_thread, NULL, NULL,
                    NULL, 5, 0, K_NO_WAIT);
    CHECK(k_sem_take(&load_entered, K_SECONDS(1)) == 0);
    monitor_writer = true;
    k_thread_create(&writer, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack), writer_thread, NULL,
                    NULL, NULL, 5, 0, K_NO_WAIT);
    /* The repaired writer may wait for settings_lock before reaching its API.
     * The original writer reaches the API while holding the state mutex. */
    int entry = k_sem_take(&writer_at_api, K_MSEC(10));
    CHECK(entry == 0 || entry == -EAGAIN);
    k_sem_give(&allow_load);
    int load_join = k_thread_join(&loader, K_MSEC(50));
    int writer_join = k_thread_join(&writer, K_MSEC(50));
    printk("race=%s holds_state=%d load_join=%d writer_join=%d load_result=%d writer_result=%d "
           "time=%lld\n",
           operation, writer_holds_state, load_join, writer_join, load_result, writer_result,
           k_uptime_get());
    if (load_join || writer_join) {
        printk("FAIL load/operation must complete; bounded diagnostic joins expired\n");
        exit(1); /* End this process; never abort a thread owning a mutex. */
    }
    CHECK(load_result == 0 && writer_result == 0);
    CHECK(value(first) == (!strcmp(operation, "reset") ? 10 : 20));
    CHECK(present[0] == (strcmp(operation, "reset") != 0));
    CHECK(value(second) == 21 && disk[1] == 21 && present[1]);
}

static void queue_cycle_case(void) {
    disk[0] = 40;
    disk[1] = 41;
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
    if (!strcmp(operation, "queue-array")) {
        const struct zmk_custom_setting *view =
            zmk_custom_setting_find_array_element("boundary", "array", 0);
        memory(view, 4);
        CHECK(zmk_custom_setting_save(&array_setting) == 0);
        memory(view, 7);
    } else if (!strcmp(operation, "queue-blob") || !strcmp(operation, "queue-keyspace")) {
        bool keyspace = !strcmp(operation, "queue-keyspace");
        const struct zmk_custom_setting *setting = &blob_setting;
        if (keyspace) {
            CHECK(zmk_custom_setting_keyspace_create(
                      &keys, "item/test", &ZMK_CUSTOM_SETTING_VALUE_BYTES(0x31, 0x31),
                      ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &setting) == 0);
        }
        uint8_t bytes[96];
        memset(bytes, 0x31, sizeof(bytes));
        size_t size = keyspace ? 2 : sizeof(bytes);
        CHECK(zmk_custom_setting_write_bytes(setting, bytes, size,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
        memset(bytes, 0x41, sizeof(bytes));
        CHECK(zmk_custom_setting_write_bytes(setting, bytes, size,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
    }
#endif
    queue_cycle = true;
    k_thread_create(&loader, load_stack, K_THREAD_STACK_SIZEOF(load_stack), load_thread, NULL, NULL,
                    NULL, 5, 0, K_NO_WAIT);
    int joined = k_thread_join(&loader, K_MSEC(200));
    printk("queue=%s joined=%d read=%d time=%lld\n", operation, joined, queue_read_result,
           k_uptime_get());
    CHECK(joined == 0 && load_result == 0 && queue_read_result == 0);
    CHECK(value(first) == 40 && value(second) == 41);
}

static void memory_guard_case(void) {
    CHECK(k_mutex_lock(&custom_settings_lock, K_FOREVER) == 0);
    CHECK(k_work_submit(&queue_read_work) > 0);
    CHECK(k_sem_take(&queue_read_entered, K_MSEC(100)) == 0);
    CHECK(queue_read_result == 99); /* The RAM read still serializes with state mutation. */
    CHECK(k_mutex_unlock(&custom_settings_lock) == 0);
    struct k_work_sync sync;
    k_work_flush(&queue_read_work, &sync);
    CHECK(queue_read_result == 0 && value(first) == 20 && value(second) == 21);
    printk("PASS memory guard retains state serialization time=%lld\n", k_uptime_get());
}

static void controls(void) {
    memory(first, 30);
    CHECK(zmk_custom_setting_discard(first) == 0 && value(first) == 20);
    CHECK(!zmk_custom_setting_has_unsaved_value(first) && initialized == 1);
    memory(first, 31);
    CHECK(zmk_custom_setting_save(first) == 0 && disk[0] == 31);
    CHECK(zmk_custom_setting_reset(first) == 0 && value(first) == 10 && !present[0]);
    CHECK(value(second) == 21 && present[1]);
    printk("PASS serialized save/discard/reset; recursive same-thread reload time=%lld\n",
           k_uptime_get());
}
static void scopes(void) {
    memory(first, 30);
    memory(second, 31);
    fail_save = 0;
    uint32_t count = 99;
    CHECK(zmk_custom_settings_save_scope("boundary", NULL, "scalar_", &count) == -EIO);
    CHECK(count == 1 && saved[0] == 1 && saved[1] == 1);
    CHECK(disk[0] == 20 && disk[1] == 31 && value(first) == 30 && value(second) == 31);
    CHECK(zmk_custom_setting_has_unsaved_value(first) &&
          !zmk_custom_setting_has_unsaved_value(second));
    CHECK(zmk_custom_settings_reset_scope("boundary", NULL, "scalar_", &count) == -EIO);
    CHECK(count == 1 && value(first) == 30 && value(second) == 11);
    CHECK(present[0] && !present[1]);
    fail_save = -1;
    CHECK(zmk_custom_settings_save_scope("boundary", "scalar_first", NULL, &count) == 0 &&
          count == 1);
    CHECK(disk[0] == 30 && !zmk_custom_setting_has_unsaved_value(first));
    printk("PASS partial scope first-error/count/state/retry time=%lld\n", k_uptime_get());
}
static void discard_error(void) {
    memory(first, 30);
    read_error = -EIO;
    int ret = zmk_custom_setting_discard(first);
    bool dirty = zmk_custom_setting_has_unsaved_value(first);
    printk("discard-read-error ret=%d value=%d disk=%d dirty=%d discarded=%u time=%lld\n", ret,
           value(first), disk[0], dirty, discarded, k_uptime_get());
    if (ret == 0 || value(first) != 30 || !dirty || discarded) {
        printk("FAIL unread storage must not report successful discard or clear dirty state\n");
        exit(1);
    }
    read_error = 0;
    CHECK(zmk_custom_setting_discard(first) == 0 && value(first) == 20);
    CHECK(!zmk_custom_setting_has_unsaved_value(first) && discarded == 1);
}
static void persist_error(void) {
    fail_save = 0;
    int ret = zmk_custom_setting_set_int32(first, 30, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    bool dirty = zmk_custom_setting_has_unsaved_value(first);
    printk("persist-error ret=%d value=%d disk=%d dirty=%d time=%lld\n", ret, value(first), disk[0],
           dirty, k_uptime_get());
    CHECK(ret == -EIO && disk[0] == 20);
    if (value(first) != 20 && !dirty) {
        printk("FAIL failed persistence left changed memory reported as saved\n");
        exit(1);
    }
    fail_save = -1;
    CHECK(zmk_custom_setting_save(first) == 0 && disk[0] == 30);
    CHECK(!zmk_custom_setting_has_unsaved_value(first));
}

static void missing(void) {
    memory(first, 30);
    present[0] = false;
    CHECK(zmk_custom_setting_discard(first) == -ENOENT);
    CHECK(value(first) == 30 && zmk_custom_setting_has_unsaved_value(first) && discarded == 0);
    present[0] = true;
    CHECK(zmk_custom_setting_discard(first) == 0 && value(first) == 20);
    printk("PASS expected record missing preserves state and retry time=%lld\n", k_uptime_get());
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
static void array_error(void) {
    const struct zmk_custom_setting *array = &array_setting;
    const struct zmk_custom_setting *view =
        zmk_custom_setting_find_array_element("boundary", "array", 0);
    memory(view, 4);
    memory(zmk_custom_setting_find_array_element("boundary", "array", 1), 5);
    CHECK(zmk_custom_setting_save(array) == 0);
    CHECK(zmk_custom_setting_array_pop_back(array, NULL, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) ==
          0);
    memory(view, 7);
    read_error = -EIO;
    CHECK(zmk_custom_setting_discard(array) == -EIO);
    CHECK(value(view) == 7 && zmk_custom_setting_has_unsaved_value(view) && discarded == 0);
    CHECK(zmk_custom_setting_array_size(array) == 1);
    read_error = 0;
    CHECK(zmk_custom_setting_discard(array) == 0 && value(view) == 4);
    CHECK(zmk_custom_setting_array_size(array) == 2);
    fail_extra = true;
    CHECK(zmk_custom_setting_set_int32(view, 8, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EIO);
    CHECK(value(view) == 8 && zmk_custom_setting_has_unsaved_value(view));
    fail_extra = false;
    CHECK(zmk_custom_setting_save(array) == 0 && !zmk_custom_setting_has_unsaved_value(view));
    /* A read failure on a later element still reports the earlier ones it restored. */
    const struct zmk_custom_setting *second =
        zmk_custom_setting_find_array_element("boundary", "array", 1);
    memory(view, 9);
    memory(second, 10);
    unsigned before = discarded;
    read_error_name = "custom_settings/boundary/array/1";
    CHECK(zmk_custom_setting_discard(array) == -EIO);
    CHECK(value(view) == 8 && value(second) == 10 && discarded == before + 1);
    read_error_name = NULL;
    CHECK(zmk_custom_setting_discard(array) == 0 && value(second) == 5);
    printk("PASS array read/save failure preserves values/dirty/size and retries time=%lld\n",
           k_uptime_get());
}

static void blob_error(bool keyspace) {
    uint8_t before[80], after[80], readback[80];
    memset(before, 0x31, sizeof(before));
    memset(after, 0x72, sizeof(after));
    const struct zmk_custom_setting *setting = &blob_setting;
    if (keyspace) {
        CHECK(zmk_custom_setting_keyspace_create(
                  &keys, "item/test", &ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                  ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &setting) == 0);
    }
    CHECK(zmk_custom_setting_write_bytes(setting, before, sizeof(before),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
    fail_extra = true;
    CHECK(zmk_custom_setting_write_bytes(setting, after, sizeof(after),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EIO);
    size_t size;
    CHECK(zmk_custom_setting_read_into(setting, readback, sizeof(readback), &size, NULL) == 0);
    CHECK(size == sizeof(after) && !memcmp(readback, after, size) &&
          zmk_custom_setting_has_unsaved_value(setting));
    fail_extra = false;
    read_error = -EIO;
    CHECK(zmk_custom_setting_discard(setting) == -EIO &&
          zmk_custom_setting_has_unsaved_value(setting));
    read_error = 0;
    CHECK(zmk_custom_setting_discard(setting) == 0);
    CHECK(zmk_custom_setting_read_into(setting, readback, sizeof(readback), &size, NULL) == 0);
    CHECK(size == sizeof(before) && !memcmp(readback, before, size) &&
          !zmk_custom_setting_has_unsaved_value(setting));
    CHECK(zmk_custom_setting_write_bytes(setting, after, sizeof(after),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
    CHECK(!zmk_custom_setting_has_unsaved_value(setting));
    printk("PASS %s large write/read errors preserve payload/dirty and retry time=%lld\n",
           keyspace ? "keyspace" : "blob", k_uptime_get());
}

static void ordinal_record(unsigned record_index, const char *ordinal, const char *public_key,
                           uint8_t payload) {
    CHECK(record_index < ARRAY_SIZE(extra));
    struct extra_record *record = &extra[record_index];
    memset(record, 0, sizeof(*record));
    int written =
        snprintf(record->name, sizeof(record->name), "custom_settings/boundary/item/#%s", ordinal);
    CHECK(written > 0 && (size_t)written < sizeof(record->name));
    size_t key_size = strlen(public_key) + 1;
    CHECK(key_size + 2 <= sizeof(record->data));
    memcpy(record->data, public_key, key_size);
    record->data[key_size] = payload;
    record->data[key_size + 1] = payload + 1;
    record->size = key_size + 2;
    record->present = true;
}

static void ordinal_payload(const struct zmk_custom_setting *setting, const char *key,
                            uint8_t expected) {
    CHECK(setting != NULL && !strcmp(zmk_custom_setting_public_key(setting), key));
    uint8_t payload[2];
    size_t size = 0;
    CHECK(zmk_custom_setting_read_into(setting, payload, sizeof(payload), &size, NULL) == 0);
    CHECK(size == 2 && payload[0] == expected && payload[1] == expected + 1);
    CHECK(!zmk_custom_setting_has_unsaved_value(setting));
}

static void array_record(unsigned record_index, const char *digits, int32_t payload) {
    CHECK(record_index < ARRAY_SIZE(extra));
    struct extra_record *record = &extra[record_index];
    *record = (struct extra_record){.present = true};
    int n =
        snprintf(record->name, sizeof(record->name), "custom_settings/boundary/array/%s", digits);
    CHECK(n > 0 && (size_t)n < sizeof(record->name));
    memcpy(record->data, &payload, sizeof(payload));
    record->size = sizeof(payload);
}

static void array_ordinal_values(int32_t zero, int32_t one) {
    const struct zmk_custom_setting *a =
        zmk_custom_setting_find_array_element("boundary", "array", 0);
    const struct zmk_custom_setting *b =
        zmk_custom_setting_find_array_element("boundary", "array", 1);
    CHECK(a != NULL && b != NULL && a != b);
    CHECK(value(a) == zero && value(b) == one);
    CHECK(!zmk_custom_setting_has_unsaved_value(a) && !zmk_custom_setting_has_unsaved_value(b));
    CHECK(zmk_custom_setting_array_size(&array_setting) == 2);
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
}

static void array_ordinals(void) {
    array_record(0, "4294967296", 90);
    CHECK(settings_load_subtree("custom_settings") == 0);
    const struct zmk_custom_setting *zero =
        zmk_custom_setting_find_array_element("boundary", "array", 0);
    printk("array first-overflow reads=%u slot0=%d time=%lld\n", extra[0].reads, value(zero),
           k_uptime_get());
    CHECK(extra[0].reads == 0);
    array_ordinal_values(1, 2);

    array_record(0, "0", 4);
    array_record(1, "1", 5);
    CHECK(settings_load_subtree("custom_settings") == 0);
    array_ordinal_values(4, 5);
    char hidden_nondigit[58];
    memset(hidden_nondigit, '0', sizeof(hidden_nondigit) - 2);
    hidden_nondigit[sizeof(hidden_nondigit) - 2] = 'x';
    hidden_nondigit[sizeof(hidden_nondigit) - 1] = '\0';
    const char *invalid[] = {
        "",
        "x",
        "-1",
        "0x0",
        "1z",
        "2",
        "2147483648",
        "4294967295",
        "4294967296",
        "4294967297",
        "18446744073709551616",
        "9999999999999999999999999999999999999999999999999999999999999999",
        hidden_nondigit,
    };
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        array_record(2, invalid[i], 90);
        CHECK(settings_load_subtree("custom_settings") == 0);
        printk("array refusal case=%u payload_reads=%u time=%lld\n", (unsigned)i, extra[2].reads,
               k_uptime_get());
        CHECK(extra[2].reads == 0);
        CHECK(zmk_custom_setting_find_array_element("boundary", "array", 0) == zero);
        array_ordinal_values(4, 5);
    }
    array_record(2, "0001", 7);
    CHECK(settings_load_subtree("custom_settings") == 0 && extra[2].reads == 1);
    array_ordinal_values(4, 7);
    array_record(2, "0000", 8);
    CHECK(settings_load_subtree("custom_settings") == 0 && extra[2].reads == 1);
    array_ordinal_values(8, 5);
    printk("PASS array ordinal load rejects overflow/truncated/non-decimal/capacity "
           "records; valid values and independent state preserved time=%lld\n",
           k_uptime_get());
}

static void rename_payload(struct zmk_custom_setting_keyspace *keyspace,
                           const struct zmk_custom_setting *setting, const char *key,
                           const uint8_t *expected, size_t expected_size, bool dirty) {
    uint8_t actual[96];
    size_t size = 0;
    CHECK(zmk_custom_setting_keyspace_find(keyspace, key) == setting);
    CHECK(!strcmp(zmk_custom_setting_public_key(setting), key));
    CHECK(zmk_custom_setting_read_into(setting, actual, sizeof(actual), &size, NULL) == 0);
    CHECK(size == expected_size && !memcmp(actual, expected, size));
    CHECK(zmk_custom_setting_has_unsaved_value(setting) == dirty);
}

static struct extra_record *rename_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(extra); i++) {
        if (extra[i].present && !strcmp(extra[i].name, name)) {
            return &extra[i];
        }
    }
    CHECK(false);
    return NULL;
}

static void rename_allocation(void) {
    const struct zmk_custom_setting *a, *b;
    uint8_t payload[] = {0x41, 0x42};
    CHECK(zmk_custom_setting_keyspace_create(&rename_keys, "rename/a",
                                             &ZMK_CUSTOM_SETTING_VALUE_BYTES(0x41, 0x42),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &a) == 0);
    CHECK(zmk_custom_setting_keyspace_create(&rename_keys, "rename/b",
                                             &ZMK_CUSTOM_SETTING_VALUE_BYTES(0x41, 0x42),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &b) == 0);
    struct extra_record *record = rename_record("custom_settings/boundary/rename/#0");
    struct extra_record durable = *record;
    unsigned before = changes;
    CHECK(zmk_custom_setting_keyspace_rename(&rename_keys, "rename/a", "rename/long",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -ENOSPC);
    CHECK(changes == before && record->size == durable.size &&
          !memcmp(record->data, durable.data, durable.size));
    rename_payload(&rename_keys, a, "rename/a", payload, sizeof(payload), false);
    rename_payload(&rename_keys, b, "rename/b", payload, sizeof(payload), false);
    CHECK(zmk_custom_setting_keyspace_find(&rename_keys, "rename/long") == NULL);
}

static void keyspace_rename_failure(bool large) {
    uint8_t payload[80], independent[] = {0x61, 0x62};
    memset(payload, 0x51, sizeof(payload));
    size_t size = large ? sizeof(payload) : 2;
    const struct zmk_custom_setting *setting, *other;
    CHECK(zmk_custom_setting_keyspace_create(&keys, "item/old",
                                             &ZMK_CUSTOM_SETTING_VALUE_BYTES(0x51, 0x51),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &setting) == 0);
    if (large) {
        CHECK(zmk_custom_setting_write_bytes(setting, payload, size,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
    }
    CHECK(zmk_custom_setting_keyspace_create(&keys, "item/other",
                                             &ZMK_CUSTOM_SETTING_VALUE_BYTES(0x61, 0x62),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &other) == 0);
    CHECK(setting == &keys.slots[0].setting && other == &keys.slots[1].setting);
    struct extra_record *record = rename_record("custom_settings/boundary/item/#0");
    struct extra_record durable = *record;
    unsigned before = changes;
    CHECK(zmk_custom_setting_keyspace_rename(NULL, "item/old", "item/new",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EINVAL);
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/old", "bad",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EINVAL);
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/missing", "item/new",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -ENOENT);
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/old", "item/other",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EEXIST);
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/old", "item/old",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EEXIST);
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/old", "item/01234567890",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) ==
          -ENAMETOOLONG);
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/old", "item/new",
                                             (enum zmk_custom_setting_write_mode) - 1) == -EINVAL);
    CHECK(changes == before && record->size == durable.size &&
          !memcmp(record->data, durable.data, durable.size));
    rename_payload(&keys, setting, "item/old", payload, size, false);
    rename_payload(&keys, other, "item/other", independent, sizeof(independent), false);

    fail_extra = true;
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/old", "item/new",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EIO);
    CHECK(changes == before && record->size == durable.size &&
          !memcmp(record->data, durable.data, durable.size));
    CHECK(zmk_custom_setting_keyspace_find(&keys, "item/old") == NULL);
    rename_payload(&keys, setting, "item/new", payload, size, true);
    rename_payload(&keys, other, "item/other", independent, sizeof(independent), false);
    printk("rename persist failure large=%d returned=-EIO RAM=item/new dirty=1 "
           "durable=item/old events=0 time=%lld\n",
           large, k_uptime_get());

    /* A successful retry saves the new RAM identity in the same ordinal. */
    fail_extra = false;
    CHECK(zmk_custom_setting_save(setting) == 0);
    CHECK(!strcmp((const char *)record->data, "item/new"));
    rename_payload(&keys, setting, "item/new", payload, size, false);
    CHECK(settings_load_subtree("custom_settings") == 0);
    rename_payload(&keys, setting, "item/new", payload, size, false);

    /* A second failed persist can instead be discarded back to the saved key. */
    before = changes;
    fail_extra = true;
    CHECK(zmk_custom_setting_keyspace_rename(&keys, "item/new", "item/next",
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == -EIO);
    CHECK(changes == before && !strcmp((const char *)record->data, "item/new"));
    rename_payload(&keys, setting, "item/next", payload, size, true);
    CHECK(zmk_custom_setting_discard(setting) == 0);
    CHECK(zmk_custom_setting_keyspace_find(&keys, "item/next") == NULL);
    rename_payload(&keys, setting, "item/new", payload, size, false);
    rename_payload(&keys, other, "item/other", independent, sizeof(independent), false);
    fail_extra = false;
    CHECK(settings_load_subtree("custom_settings") == 0);
    rename_payload(&keys, setting, "item/new", payload, size, false);
    rename_payload(&keys, other, "item/other", independent, sizeof(independent), false);
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    if (!large) {
        rename_allocation();
    }
    printk("PASS rename validation/allocation refusal unchanged; persist failure "
           "keeps dirty RAM and save/discard/reload recover large=%d time=%lld\n",
           large, k_uptime_get());
}

static void keyspace_ordinals(void) {
    /* Exercise the real Zephyr settings dispatch and production loader. The
     * framework ignores unknown-record errors, so refusal means no payload
     * callback and no slot/value mutation, not a nonzero settings_load result. */
    ordinal_record(0, "4294967296", "item/poison", 0x90);
    CHECK(settings_load_subtree("custom_settings") == 0);
    printk("ordinal first-overflow reads=%u slot0=%d slot1=%d time=%lld\n", extra[0].reads,
           keys.slots[0].in_use, keys.slots[1].in_use, k_uptime_get());
    CHECK(extra[0].reads == 0);
    CHECK(!keys.slots[0].in_use && !keys.slots[1].in_use);
    CHECK(zmk_custom_setting_keyspace_find(&keys, "item/poison") == NULL);

    ordinal_record(0, "0", "item/zero", 0x10);
    ordinal_record(1, "1", "item/one", 0x20);
    CHECK(settings_load_subtree("custom_settings") == 0);
    const struct zmk_custom_setting *zero = zmk_custom_setting_keyspace_find(&keys, "item/zero");
    const struct zmk_custom_setting *one = zmk_custom_setting_keyspace_find(&keys, "item/one");
    CHECK(zero == &keys.slots[0].setting && one == &keys.slots[1].setting);
    ordinal_payload(zero, "item/zero", 0x10);
    ordinal_payload(one, "item/one", 0x20);

    const char *invalid[] = {
        "",
        "x",
        "-1",
        "0x0",
        "1z",
        "2",
        "2147483648",
        "4294967295",
        "4294967296",
        "4294967297",
        "18446744073709551616",
        "9999999999999999999999999999999999999999999999999999999999999999",
    };
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        ordinal_record(2, invalid[i], "item/poison", 0x90);
        CHECK(settings_load_subtree("custom_settings") == 0);
        printk("ordinal refusal case=%u payload_reads=%u time=%lld\n", (unsigned)i, extra[2].reads,
               k_uptime_get());
        CHECK(extra[2].reads == 0);
        CHECK(zmk_custom_setting_keyspace_find(&keys, "item/poison") == NULL);
        CHECK(zmk_custom_setting_keyspace_find(&keys, "item/zero") == zero);
        CHECK(zmk_custom_setting_keyspace_find(&keys, "item/one") == one);
        ordinal_payload(zero, "item/zero", 0x10);
        ordinal_payload(one, "item/one", 0x20);
        CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    }

    /* Decimal leading zeros remain supported for both valid boundary slots. */
    ordinal_record(2, "0001", "item/one", 0x22);
    CHECK(settings_load_subtree("custom_settings") == 0 && extra[2].reads == 1);
    ordinal_payload(one, "item/one", 0x22);
    ordinal_payload(zero, "item/zero", 0x10);
    ordinal_record(2, "0000", "item/zero", 0x12);
    CHECK(settings_load_subtree("custom_settings") == 0 && extra[2].reads == 1);
    ordinal_payload(zero, "item/zero", 0x12);
    ordinal_payload(one, "item/one", 0x20);
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    printk("PASS keyspace ordinal load rejects malformed/overflow/capacity records; "
           "valid boundary slots and independent state preserved time=%lld\n",
           k_uptime_get());
}
#endif

int main(void) {
    CHECK(settings_subsys_init() == 0);
    settings_src_register(&backend);
    settings_dst_register(&backend);
    CHECK(settings_load_subtree("custom_settings") == 0);
    CHECK(value(first) == 20 && value(second) == 21 && initialized == 1);
    operation = getenv("CUSTOM_SETTINGS_CASE");
    CHECK(operation != NULL);
    if (!strcmp(operation, "control")) {
        controls();
    } else if (!strcmp(operation, "queue-scalar")) {
        queue_cycle_case();
    } else if (!strcmp(operation, "memory-guard")) {
        memory_guard_case();
    } else if (!strcmp(operation, "scope")) {
        scopes();
    } else if (!strcmp(operation, "discard-error")) {
        discard_error();
    } else if (!strcmp(operation, "persist-error")) {
        persist_error();
    } else if (!strcmp(operation, "missing")) {
        missing();
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
    } else if (!strcmp(operation, "queue-array") || !strcmp(operation, "queue-blob") ||
               !strcmp(operation, "queue-keyspace")) {
        queue_cycle_case();
    } else if (!strcmp(operation, "array-error")) {
        array_error();
    } else if (!strcmp(operation, "blob-error")) {
        blob_error(false);
    } else if (!strcmp(operation, "keyspace-error")) {
        blob_error(true);
    } else if (!strcmp(operation, "keyspace-ordinal")) {
        keyspace_ordinals();
    } else if (!strcmp(operation, "array-ordinal")) {
        array_ordinals();
    } else if (!strcmp(operation, "rename-error") || !strcmp(operation, "rename-large-error")) {
        keyspace_rename_failure(!strcmp(operation, "rename-large-error"));
#endif
    } else {
        CHECK(!strcmp(operation, "save") || !strcmp(operation, "discard") ||
              !strcmp(operation, "reset"));
        race();
    }
    exit(0);
}
