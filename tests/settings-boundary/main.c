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
#endif

static struct extra_record {
    char name[96];
    uint8_t data[192];
    size_t size;
    bool present;
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
static unsigned saved[2], discarded, initialized;
static const char *operation;

static int observe(const zmk_event_t *event) {
    const struct zmk_custom_setting_changed *change = as_zmk_custom_setting_changed(event);
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
    if (read_error) {
        return read_error;
    }
    struct extra_record *record = context;
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
    } else if (!strcmp(operation, "scope")) {
        scopes();
    } else if (!strcmp(operation, "discard-error")) {
        discard_error();
    } else if (!strcmp(operation, "persist-error")) {
        persist_error();
    } else if (!strcmp(operation, "missing")) {
        missing();
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
    } else if (!strcmp(operation, "array-error")) {
        array_error();
    } else if (!strcmp(operation, "blob-error")) {
        blob_error(false);
    } else if (!strcmp(operation, "keyspace-error")) {
        blob_error(true);
#endif
    } else {
        CHECK(!strcmp(operation, "save") || !strcmp(operation, "discard") ||
              !strcmp(operation, "reset"));
        race();
    }
    exit(0);
}
