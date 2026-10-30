/* SPDX-License-Identifier: MIT */
static int custom_settings_chunk_identity_case(void) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
    struct zmk_custom_setting_keyspace *keyspace =
        zmk_custom_settings_keyspace_find_for_key("test", "blob/epoch");
    if (!keyspace) {
        return -ENOENT;
    }
    const struct zmk_custom_setting *old = NULL, *replacement = NULL;
    struct zmk_custom_settings_setting_ref ref = {.has_custom_subsystem_id = true, .has_key = true};
    copy_string(ref.custom_subsystem_id, sizeof(ref.custom_subsystem_id), "test");
    copy_string(ref.key, sizeof(ref.key), "blob/epoch");
    cormoran_zmk_custom_settings_Response resp = cormoran_zmk_custom_settings_Response_init_zero;
    uint8_t part = 9;
    /* Keep this setup from adding notifications to earlier snapshot contracts. */
    zmk_custom_settings_notify_suppress_begin();
    int ret = zmk_custom_setting_keyspace_create(keyspace, "blob/epoch",
                                                 &ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &old);
    if (!ret) {
        ret = handle_private_write_value_chunk(
            &ref, 2, 0, &part, 1, false,
            cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp);
    }
    if (!ret) {
        ret = zmk_custom_setting_keyspace_delete(keyspace, "blob/epoch");
    }
    if (!ret) {
        ret = zmk_custom_setting_keyspace_create(
            keyspace, "blob/epoch", &ZMK_CUSTOM_SETTING_VALUE_BYTES(2),
            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &replacement);
    }
    if (!ret && old != replacement) {
        ret = -EINVAL;
    }
    if (!ret) {
        part = 8;
        resp =
            (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
        int committed = handle_private_write_value_chunk(
            &ref, 2, 1, &part, 1, true,
            cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp);
        struct zmk_custom_setting_value actual;
        if (committed != -EAGAIN || zmk_custom_setting_read(replacement, &actual) ||
            actual.size != 1 || actual.bytes_value[0] != 2) {
            LOG_ERR("Stale chunk committed to reused keyspace slot ret=%d", committed);
            ret = -EINVAL;
        }
    }
    if (replacement) {
        int cleanup = zmk_custom_setting_keyspace_delete(keyspace, "blob/epoch");
        if (!ret) {
            ret = cleanup;
        }
    }
    zmk_custom_settings_notify_suppress_end();
    return ret;
#else
    return 0;
#endif
}
