/* SPDX-License-Identifier: MIT */
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
static int custom_settings_async_notification_test_init(void) {
    const struct zmk_custom_setting *array = zmk_custom_setting_find_array("test", "array_value");
    const struct zmk_custom_setting *target =
        zmk_custom_setting_find_array_element("test", "array_value", 0);
    if (!array || !target) {
        return -ENOENT;
    }
    static struct zmk_custom_setting_array_state
        churn_states[CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE];
    const struct zmk_custom_setting *held[ARRAY_SIZE(churn_states)];
    struct zmk_custom_setting churn = *array;
    churn.array_key = "churn";
    bool was_unlocked = studio_is_unlocked();
    zmk_studio_core_unlock();
    k_msgq_purge(&notification_msgq);
    for (unsigned int control = 0; control < 2; control++) {
        target = zmk_custom_setting_find_array_element("test", "array_value", 0);
        if (!target) {
            return -ENOMEM;
        }
        raise_zmk_custom_setting_changed(
            (struct zmk_custom_setting_changed){.setting = target,
                                                .kind = ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                                .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL});
        if (control) {
            /* Unregistered synthetic identities force genuine view-pool
             * eviction without adding settings to any existing list fixture. */
            custom_settings_memory_lock();
            for (size_t i = 0; i < ARRAY_SIZE(churn_states); i++) {
                churn_states[i] = *array->array_state;
                churn.array_state = &churn_states[i];
                held[i] = array_view_acquire(&churn, 0);
                if (!held[i]) {
                    for (size_t j = 0; j < i; j++) {
                        array_view_release_locked(held[j]);
                    }
                    custom_settings_memory_unlock();
                    return -ENOMEM;
                }
                array_view_retain_locked(held[i]);
            }
            bool reused = strcmp(zmk_custom_setting_public_key(target), "churn") == 0;
            for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
                array_view_release_locked(held[i]);
            }
            custom_settings_memory_unlock();
            if (!reused) {
                return -EINVAL;
            }
        }
        notification_work_handler(NULL);
        const cormoran_zmk_custom_settings_Setting *sent =
            &notification_buffer.notification_type.setting.setting;
        if (strcmp(sent->key, "array_value") || !sent->has_value ||
            sent->value.which_value_type !=
                cormoran_zmk_custom_settings_SettingValue_array_value_tag ||
            sent->value.value_type.array_value.index != 0) {
            return -EINVAL;
        }
    }
    if (!was_unlocked) {
        zmk_studio_core_lock();
    }
    LOG_INF("PASS: deferred array notification identity and serial control");
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_async_notification_test_init, APPLICATION, 99);
#endif
