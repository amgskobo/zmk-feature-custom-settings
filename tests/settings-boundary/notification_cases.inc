/* SPDX-License-Identifier: MIT */
/* Real mutation/lookup/event APIs; only the unlock handoff is gated. */
#include "../../src/custom_settings_internal.h"
static bool notification_expect_array, notification_nested;
static unsigned notification_array_events;
static const struct zmk_custom_setting_array_state *notification_state;
static uint32_t notification_index;
static int32_t notification_value;
static enum zmk_custom_setting_changed_kind notification_kind;
static int notification_target_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *event = as_zmk_custom_setting_changed(eh);
    if (notification_expect_array && event) {
        CHECK(event->setting != NULL);
        CHECK(event->setting->array_state == notification_state);
        CHECK(event->setting->array_index == notification_index);
        CHECK(event->kind == notification_kind);
        if (notification_nested) {
            mutation_churn_views(NULL);
            CHECK(event->setting->array_state == notification_state);
            CHECK(event->setting->array_index == notification_index);
        }
        struct zmk_custom_setting_value readback;
        CHECK(zmk_custom_setting_read(event->setting, &readback) == 0);
        CHECK(readback.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32);
        CHECK(readback.int32_value == notification_value);
        notification_array_events++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(notification_target, notification_target_listener);
ZMK_SUBSCRIPTION(notification_target, zmk_custom_setting_changed);
static void notification_cases(const char *name) {
    notification_state = mutation_array.array_state;
    notification_index = 0;
    notification_value = 111;
    notification_kind = ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED;
    notification_nested = !strcmp(name, "notify-nested") || !strcmp(name, "notify-temporary") ||
                          !strcmp(name, "notify-rollback") || !strcmp(name, "notify-persist");
    if (!strcmp(name, "notify-exhaustion")) {
        const struct zmk_custom_setting *held[CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE];
        CHECK(ARRAY_SIZE(held) < mutation_churn_array.array_state->max_size);
        custom_settings_state_lock();
        for (uint32_t i = 0; i < ARRAY_SIZE(held); i++) {
            held[i] = zmk_custom_setting_find_array_element("mutations", "churn", i);
            CHECK(held[i] != NULL);
            array_view_retain_locked(held[i]);
        }
        CHECK(zmk_custom_setting_find_array_element("mutations", "churn", ARRAY_SIZE(held)) ==
              NULL);
        CHECK(zmk_custom_setting_array_push_back(&mutation_array,
                                                 &ZMK_CUSTOM_SETTING_VALUE_INT32(111),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == -ENOMEM);
        CHECK(zmk_custom_setting_array_size(&mutation_array) == 0);
        CHECK(notification_array_events == 0);
        for (uint32_t i = 0; i < ARRAY_SIZE(held); i++) {
            CHECK(held[i]->array_state == mutation_churn_array.array_state);
            CHECK(held[i]->array_index == i);
            array_view_release_locked(held[i]);
        }
        custom_settings_state_unlock();
        notification_expect_array = true;
        mutation_push(111, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    } else if (!strcmp(name, "notify-view")) {
        notification_expect_array = true;
        mutation_start("push");
        mutation_churn_views(NULL);
        mutation_join();
    } else if (!strcmp(name, "notify-temporary") || !strcmp(name, "notify-rollback")) {
        mutation_push(111, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        const struct zmk_custom_setting *view =
            zmk_custom_setting_find_array_element("mutations", "array", 0);
        if (!strcmp(name, "notify-rollback")) {
            CHECK(zmk_custom_setting_write(view, &ZMK_CUSTOM_SETTING_VALUE_INT32(333),
                                           ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) == 0);
            notification_expect_array = true;
            CHECK(zmk_custom_setting_rollback_temporary(view) == 0);
        } else {
            notification_value = 333;
            notification_expect_array = true;
            CHECK(zmk_custom_setting_write_array_by_key(
                      "mutations", "array", 0, &ZMK_CUSTOM_SETTING_VALUE_INT32(333),
                      ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) == 0);
        }
    } else if (!strcmp(name, "notify-persist")) {
        for (int32_t i = 0; i < 20; i++) {
            CHECK(zmk_custom_setting_array_push_back(&mutation_churn_array,
                                                     &ZMK_CUSTOM_SETTING_VALUE_INT32(i),
                                                     ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        }
        notification_state = mutation_churn_array.array_state;
        notification_index = 17;
        notification_value = 999;
        notification_kind = ZMK_CUSTOM_SETTING_CHANGED_SAVED;
        notification_expect_array = true;
        CHECK(zmk_custom_setting_write_array_by_key("mutations", "churn", 17,
                                                    &ZMK_CUSTOM_SETTING_VALUE_INT32(999),
                                                    ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
    } else {
        CHECK(!strcmp(name, "notify-control") || !strcmp(name, "notify-nested"));
        mutation_churn_views(NULL);
        notification_expect_array = true;
        mutation_push(111, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    }
    notification_expect_array = false;
    CHECK(notification_array_events == 1);
    struct zmk_custom_setting_value readback;
    const char *key = !strcmp(name, "notify-persist") ? "churn" : "array";
    CHECK(zmk_custom_setting_read_array_by_key("mutations", key, notification_index, &readback) ==
          0);
    CHECK(readback.int32_value == notification_value);
    CHECK(zmk_custom_setting_array_size(&array_setting) == 2);
    CHECK(zmk_custom_setting_read(first, &readback) == 0 && readback.int32_value == 20);
    /* A completed dispatch must release its view, so the bounded pool can
     * recycle that address for another identity after later lookups. */
    const struct zmk_custom_setting *completed =
        zmk_custom_setting_find_array_element("mutations", key, notification_index);
    bool recycled = false;
    for (uint32_t i = 0; i < 20; i++) {
        const struct zmk_custom_setting *other =
            zmk_custom_setting_find_array_element("mutations", "churn", i);
        CHECK(other != NULL);
        recycled |= other == completed && strcmp(key, "churn");
    }
    if (strcmp(key, "churn")) {
        CHECK(recycled);
    }
}
