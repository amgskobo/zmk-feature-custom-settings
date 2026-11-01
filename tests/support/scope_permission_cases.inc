/* SPDX-License-Identifier: MIT */
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(scope_secure_keys, "test", "guard/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, 4, 24, 1,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                   ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static int custom_settings_scope_permission_cases(void) {
    bool was_unlocked = studio_is_unlocked();
    const struct zmk_custom_setting *entry = NULL;
    int ret = zmk_custom_setting_keyspace_create(&scope_secure_keys, "guard/entry",
                                                 &ZMK_CUSTOM_SETTING_VALUE_BYTES(42),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &entry);
    if (ret) {
        return ret;
    }
    zmk_studio_core_lock();
    const uint32_t tags[] = {cormoran_zmk_custom_settings_Request_save_settings_tag,
                             cormoran_zmk_custom_settings_Request_discard_settings_tag,
                             cormoran_zmk_custom_settings_Request_reset_settings_tag};
    for (size_t i = 0; i < ARRAY_SIZE(tags); i++) {
        for (unsigned filter = 0; filter < 4; filter++) {
            cormoran_zmk_custom_settings_Request req =
                cormoran_zmk_custom_settings_Request_init_zero;
            req.which_request_type = tags[i];
            cormoran_zmk_custom_settings_SettingScope *scope;
            if (i == 0) {
                req.request_type.save_settings.has_scope = true;
                scope = &req.request_type.save_settings.scope;
            } else if (i == 1) {
                req.request_type.discard_settings.has_scope = true;
                scope = &req.request_type.discard_settings.scope;
            } else {
                req.request_type.reset_settings.has_scope = true;
                scope = &req.request_type.reset_settings.scope;
            }
            if (filter < 2) {
                scope->has_key = true;
                copy_string(scope->key, sizeof(scope->key), filter ? "guard/#0" : "guard/entry");
            } else {
                scope->has_key_prefix = true;
                copy_string(scope->key_prefix, sizeof(scope->key_prefix),
                            filter == 2 ? "gua" : "guard/en");
            }
            uint8_t wire[128];
            pb_ostream_t stream = pb_ostream_from_buffer(wire, sizeof(wire));
            cormoran_zmk_custom_settings_Request decoded;
            cormoran_zmk_custom_settings_Response resp =
                cormoran_zmk_custom_settings_Response_init_zero;
            if (!pb_encode(&stream, cormoran_zmk_custom_settings_Request_fields, &req) ||
                !decode_custom_settings_request(wire, stream.bytes_written, &decoded) ||
                process_request(&decoded, &resp) ||
                resp.which_response_type != cormoran_zmk_custom_settings_Response_error_tag ||
                zmk_custom_setting_keyspace_find(&scope_secure_keys, "guard/entry") != entry) {
                ret = -EINVAL;
                goto done;
            }
            struct zmk_custom_setting_value value;
            if (zmk_custom_setting_read(entry, &value) || value.size != 1 ||
                value.bytes_value[0] != 42 || zmk_custom_setting_has_unsaved_value(entry)) {
                ret = -EINVAL;
                goto done;
            }
        }
    }
    /* An empty namespace must remain protected against a concurrent create. */
    ret = zmk_custom_setting_keyspace_delete(&scope_secure_keys, "guard/entry");
    struct zmk_custom_settings_setting_scope scope = {.has_key_prefix = true};
    copy_string(scope.key_prefix, sizeof(scope.key_prefix), "guard/");
    if (!ret && !scope_write_unlock_required(&scope)) {
        ret = -EINVAL;
    }
    /* Independent namespaces retain their normal locked/unlocked behavior. */
    copy_string(scope.key_prefix, sizeof(scope.key_prefix), "unrelated/");
    if (!ret && scope_write_unlock_required(&scope)) {
        ret = -EINVAL;
    }
    zmk_studio_core_unlock();
    copy_string(scope.key_prefix, sizeof(scope.key_prefix), "guard/");
    if (!ret && scope_write_unlock_required(&scope)) {
        ret = -EINVAL;
    }
done:
    zmk_custom_setting_keyspace_delete(&scope_secure_keys, "guard/entry");
    if (was_unlocked) {
        zmk_studio_core_unlock();
    } else {
        zmk_studio_core_lock();
    }
    return ret;
}
#else
static int custom_settings_scope_permission_cases(void) { return 0; }
#endif
