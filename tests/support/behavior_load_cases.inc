/* SPDX-License-Identifier: MIT */
/* Persisted local-ID ordering regression, adapted from upstream PR #58. */
static int test_persisted_behavior_before_ids(void) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    const struct zmk_custom_setting *behavior = zmk_custom_setting_find("test", "behavior_value");
    const struct zmk_custom_setting *id = zmk_custom_setting_find("test", "behavior_id");
    if (!behavior || !id) {
        return -ENOENT;
    }
    const zmk_behavior_local_id_t unresolved = UINT16_MAX - 1;
    struct zmk_custom_setting_behavior_value expected = {.behavior_id = unresolved};
    /* Runtime mutations still require a resolved binding and valid params. */
    if (zmk_custom_setting_set_behavior(behavior, expected, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) !=
            -EINVAL ||
        zmk_custom_setting_write(id, &ZMK_CUSTOM_SETTING_VALUE_INT32(unresolved),
                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) != -EINVAL) {
        LOG_ERR("Unresolved runtime behavior write was accepted");
        return -EINVAL;
    }
    zmk_custom_settings_notify_suppress_begin();
    int ret = test_settings_save(NULL, "custom_settings/test/behavior_value",
                                 (const char *)&unresolved, sizeof(unresolved));
    int32_t stored = unresolved;
    if (!ret) {
        ret = test_settings_save(NULL, "custom_settings/test/behavior_id", (const char *)&stored,
                                 sizeof(stored));
    }
    if (!ret) {
        ret = settings_load_subtree("custom_settings");
    }
    struct zmk_custom_setting_value readback;
    if (!ret &&
        (expect_behavior_value(behavior, expected) || zmk_custom_setting_read(id, &readback) ||
         readback.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ||
         readback.int32_value != unresolved)) {
        LOG_ERR("Persisted unresolved behavior was dropped before ID mapping");
        ret = -EINVAL;
    }
    int cleanup_behavior = zmk_custom_setting_reset(behavior);
    int cleanup_id = zmk_custom_setting_reset(id);
    zmk_custom_settings_notify_suppress_end();
    if (ret) {
        return ret;
    }
    if (cleanup_behavior || cleanup_id || initialized_event_count != 1) {
        return -EINVAL;
    }
    printk("OWNER_BEHAVIOR_LOAD_CASE_COMPLETE\n");
#endif
    return 0;
}
