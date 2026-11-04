/* SPDX-License-Identifier: MIT */
/* Real registry, lock state and nanopb callbacks; no transport acceptance. */
static int custom_settings_review_notification_gate(void) {
    const struct zmk_custom_setting *base = zmk_custom_setting_find("test", "bytes_value");
    if (!base) {
        return -ENOENT;
    }
    struct zmk_custom_setting secure = *base;
    secure.read_permission = ZMK_CUSTOM_SETTING_PERMISSION_SECURE;
    cormoran_zmk_custom_settings_Notification notification =
        cormoran_zmk_custom_settings_Notification_init_zero;
    notification.which_notification_type = cormoran_zmk_custom_settings_Notification_setting_tag;
    notification.notification_type.setting.has_setting = true;
    bool was_unlocked = studio_is_unlocked();
    zmk_studio_core_unlock();
    int ret = setting_to_proto(&secure, &notification.notification_type.setting.setting, true,
                               false, false, ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret) {
        return ret;
    }
    struct setting_notification_payload gated = {.notification = &notification, .setting = &secure};
    zmk_custom_CustomNotification envelope = zmk_custom_CustomNotification_init_zero;
    envelope.payload.funcs.encode = encode_current_setting_notification;
    envelope.payload.arg = &gated;
    uint8_t wire[192];
    pb_ostream_t stream = pb_ostream_from_buffer(wire, sizeof(wire));
    bool unlocked_ok = pb_encode(&stream, zmk_custom_CustomNotification_fields, &envelope);
    // Model the transport wait between value admission and encoder invocation.
    zmk_studio_core_lock();
    stream = pb_ostream_from_buffer(wire, sizeof(wire));
    bool locked_sent = pb_encode(&stream, zmk_custom_CustomNotification_fields, &envelope);
    stream = pb_ostream_from_buffer(wire, sizeof(wire));
    bool scalar_sent =
        pb_encode(&stream, cormoran_zmk_custom_settings_Notification_fields, &notification);
    notification.notification_type.setting.setting.has_value = false;
    stream = pb_ostream_from_buffer(wire, sizeof(wire));
    bool metadata_ok = pb_encode(&stream, zmk_custom_CustomNotification_fields, &envelope);
    if (was_unlocked) {
        zmk_studio_core_unlock();
    }
    if (!unlocked_ok || locked_sent || scalar_sent || !metadata_ok) {
        return -EINVAL;
    }
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
static bool review_write_guard_seen;
static bool review_write_target_stable;
static void review_churn_write_target(const struct zmk_custom_setting *target) {
    review_write_guard_seen = custom_settings_lock.owner == k_current_get();
    const struct zmk_custom_setting *array = zmk_custom_setting_find_array("test", "array_value");
    struct zmk_custom_setting churn = *array;
    static struct zmk_custom_setting_array_state
        states[CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE];
    const struct zmk_custom_setting *held[ARRAY_SIZE(states)];
    size_t count = 0;
    churn.array_key = "review-churn";
    for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
        states[i] = *array->array_state;
        churn.array_state = &states[i];
        const struct zmk_custom_setting *view = array_view_acquire(&churn, 0);
        if (!view) {
            break;
        }
        held[count++] = view;
        array_view_retain_locked(view);
    }
    review_write_target_stable = target->array_state == array->array_state &&
                                 target->array_index == 0 && count < ARRAY_SIZE(states);
    while (count) {
        array_view_release_locked(held[--count]);
    }
}

static int custom_settings_review_write_view(void) {
    struct zmk_custom_setting_value before, after;
    if (zmk_custom_setting_read_array_by_key("test", "array_value", 0, &before)) {
        return -ENOENT;
    }
    struct zmk_custom_settings_setting_ref ref = {.has_custom_subsystem_id = true,
                                                  .has_key = true,
                                                  .has_array_index = true,
                                                  .array_index = 0};
    copy_string(ref.custom_subsystem_id, sizeof(ref.custom_subsystem_id), "test");
    copy_string(ref.key, sizeof(ref.key), "array_value");
    cormoran_zmk_custom_settings_Response response =
        cormoran_zmk_custom_settings_Response_init_zero;
    bool was_unlocked = studio_is_unlocked();
    zmk_studio_core_unlock();
    zmk_custom_settings_notify_suppress_begin();
    write_target_observer_for_test = review_churn_write_target;
    int ret = handle_private_write_setting(
        &ref, &before, false, 0, 0,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &response, false);
    write_target_observer_for_test = NULL;
    zmk_custom_settings_notify_suppress_end();
    if (!was_unlocked) {
        zmk_studio_core_lock();
    }
    if (ret || !review_write_guard_seen || !review_write_target_stable ||
        zmk_custom_setting_read_array_by_key("test", "array_value", 0, &after) ||
        after.int32_value != before.int32_value) {
        return -EINVAL;
    }
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE) &&                                             \
    IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS)
static int review_replace_during_conversion(const struct zmk_custom_setting *setting,
                                            const uint8_t *src, size_t src_size, uint8_t *dest,
                                            size_t *dest_size, size_t capacity) {
    struct zmk_custom_setting_keyspace *space =
        (struct zmk_custom_setting_keyspace *)setting->_keyspace;
    int ret = zmk_custom_setting_keyspace_delete(space, "blob/review-old");
    if (!ret) {
        ret = zmk_custom_setting_keyspace_create(space, "blob/review-new",
                                                 &ZMK_CUSTOM_SETTING_VALUE_BYTES(0x55),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    }
    if (ret || src_size > capacity) {
        return ret ? ret : -EMSGSIZE;
    }
    memcpy(dest, src, src_size);
    *dest_size = src_size;
    return 0;
}

static int custom_settings_review_reused_slot(void) {
    struct zmk_custom_setting_keyspace *space =
        zmk_custom_settings_keyspace_find_for_key("test", "blob/review-old");
    if (!space) {
        return -ENOENT;
    }
    const struct zmk_custom_setting *old = NULL;
    zmk_custom_settings_notify_suppress_begin();
    int ret = zmk_custom_setting_keyspace_create(space, "blob/review-old",
                                                 &ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &old);
    if (ret) {
        zmk_custom_settings_notify_suppress_end();
        return ret;
    }
    bool was_unlocked = studio_is_unlocked();
    zmk_studio_core_unlock();
    zmk_custom_setting_rpc_bytes_converter_t original = space->rpc_deserializer;
    space->rpc_deserializer = review_replace_during_conversion;
    struct zmk_custom_settings_setting_ref ref = {.has_custom_subsystem_id = true, .has_key = true};
    copy_string(ref.custom_subsystem_id, sizeof(ref.custom_subsystem_id), "test");
    copy_string(ref.key, sizeof(ref.key), "blob/review-old");
    cormoran_zmk_custom_settings_Response response =
        cormoran_zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_setting(
        &ref, &ZMK_CUSTOM_SETTING_VALUE_BYTES(9), false, 0, 0,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &response, true);
    space->rpc_deserializer = original;
    const struct zmk_custom_setting *replacement =
        zmk_custom_setting_keyspace_find(space, "blob/review-new");
    struct zmk_custom_setting_value value;
    bool preserved = ret == -EAGAIN && replacement == old &&
                     zmk_custom_setting_read(replacement, &value) == 0 && value.size == 1 &&
                     value.bytes_value[0] == 0x55;
    zmk_custom_setting_keyspace_delete(space, "blob/review-new");
    zmk_custom_settings_notify_suppress_end();
    if (!was_unlocked) {
        zmk_studio_core_lock();
    }
    return preserved ? 0 : -EINVAL;
}
#endif

static int custom_settings_review_rpc_cases(void) {
    int ret = custom_settings_review_notification_gate();
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
    if (!ret) {
        ret = custom_settings_review_write_view();
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE) &&                                             \
    IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS)
    if (!ret) {
        ret = custom_settings_review_reused_slot();
    }
#endif
    return ret;
}
