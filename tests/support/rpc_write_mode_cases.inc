/* SPDX-License-Identifier: MIT */
/* Unknown enum numbers must survive real encoding/decoding as invalid input,
 * rather than become a memory write. No valid wire identifier is changed. */
static int custom_settings_invalid_write_modes(void) {
    const struct zmk_custom_setting *scalar = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *array = zmk_custom_setting_find_array("test", "array_value");
    struct zmk_custom_setting_value before, after;
    if (!scalar || zmk_custom_setting_read(scalar, &before)) {
        return -ENOENT;
    }
    uint32_t length = array ? zmk_custom_setting_array_size(array) : 0;
    const pb_size_t tags[] = {
        cormoran_zmk_custom_settings_Request_write_setting_tag,
        cormoran_zmk_custom_settings_Request_push_back_array_tag,
        cormoran_zmk_custom_settings_Request_pop_back_array_tag,
        cormoran_zmk_custom_settings_Request_create_setting_tag,
        cormoran_zmk_custom_settings_Request_write_value_chunk_tag,
    };
    const uint32_t invalid[] = {2, 99, 0x7fffffff};
    uint8_t data[] = {1, 2, 3};
    struct bounded_decode_scratch payload = {
        .buf = data, .capacity = sizeof(data), .size = sizeof(data)};
    for (size_t t = 0; t < ARRAY_SIZE(tags); t++) {
        if ((!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) &&
             (tags[t] == cormoran_zmk_custom_settings_Request_push_back_array_tag ||
              tags[t] == cormoran_zmk_custom_settings_Request_pop_back_array_tag)) ||
            (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE) &&
             tags[t] == cormoran_zmk_custom_settings_Request_create_setting_tag) ||
            (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_CHUNKED_RPC) &&
             tags[t] == cormoran_zmk_custom_settings_Request_write_value_chunk_tag)) {
            continue;
        }
        for (size_t m = 0; m < ARRAY_SIZE(invalid); m++) {
            cormoran_zmk_custom_settings_Request req =
                cormoran_zmk_custom_settings_Request_init_zero;
            cormoran_zmk_custom_settings_SettingRef *ref = NULL;
            cormoran_zmk_custom_settings_SettingWriteMode *mode = NULL;
            const char *key = "int_value";
            req.which_request_type = tags[t];
            switch (tags[t]) {
            case cormoran_zmk_custom_settings_Request_write_setting_tag:
                req.request_type.write_setting.has_setting = true;
                ref = &req.request_type.write_setting.setting;
                mode = &req.request_type.write_setting.mode;
                req.request_type.write_setting.has_value = true;
                req.request_type.write_setting.value.which_value_type =
                    cormoran_zmk_custom_settings_SettingValue_int32_value_tag;
                req.request_type.write_setting.value.value_type.int32_value =
                    before.int32_value == 91 ? 92 : 91;
                break;
            case cormoran_zmk_custom_settings_Request_push_back_array_tag:
                req.request_type.push_back_array.has_setting = true;
                ref = &req.request_type.push_back_array.setting;
                mode = &req.request_type.push_back_array.mode;
                key = "array_value";
                req.request_type.push_back_array.has_value = true;
                req.request_type.push_back_array.value.which_value_type =
                    cormoran_zmk_custom_settings_SettingScalarValue_int32_value_tag;
                req.request_type.push_back_array.value.value_type.int32_value = 91;
                break;
            case cormoran_zmk_custom_settings_Request_pop_back_array_tag:
                req.request_type.pop_back_array.has_setting = true;
                ref = &req.request_type.pop_back_array.setting;
                mode = &req.request_type.pop_back_array.mode;
                key = "array_value";
                break;
            case cormoran_zmk_custom_settings_Request_create_setting_tag:
                req.request_type.create_setting.has_setting = true;
                ref = &req.request_type.create_setting.setting;
                mode = &req.request_type.create_setting.mode;
                key = "blob/mode";
                req.request_type.create_setting.has_value = true;
                req.request_type.create_setting.value.which_value_type =
                    cormoran_zmk_custom_settings_SettingValue_bytes_value_tag;
                req.request_type.create_setting.value.value_type.bytes_value.funcs.encode =
                    encode_bounded_scratch_value;
                req.request_type.create_setting.value.value_type.bytes_value.arg = &payload;
                break;
            default:
                req.request_type.write_value_chunk.has_setting = true;
                ref = &req.request_type.write_value_chunk.setting;
                mode = &req.request_type.write_value_chunk.mode;
                key = "bytes_value";
                req.request_type.write_value_chunk.total_size = sizeof(data);
                req.request_type.write_value_chunk.data.size = sizeof(data);
                memcpy(req.request_type.write_value_chunk.data.bytes, data, sizeof(data));
                req.request_type.write_value_chunk.commit = true;
                break;
            }
            ref->has_key = true;
            copy_string(ref->key, sizeof(ref->key), key);
            *mode = (cormoran_zmk_custom_settings_SettingWriteMode)invalid[m];
            uint8_t wire[160];
            pb_ostream_t stream = pb_ostream_from_buffer(wire, sizeof(wire));
            if (!pb_encode(&stream, cormoran_zmk_custom_settings_Request_fields, &req)) {
                return -EIO;
            }
            cormoran_zmk_custom_settings_Request decoded;
            if (!decode_custom_settings_request(wire, stream.bytes_written, &decoded)) {
                return -EIO;
            }
            cormoran_zmk_custom_settings_Response resp =
                cormoran_zmk_custom_settings_Response_init_zero;
            int ret = process_request(&decoded, &resp);
            if (ret != -EINVAL || zmk_custom_setting_read(scalar, &after) ||
                after.int32_value != before.int32_value ||
                (array && zmk_custom_setting_array_size(array) != length) ||
                zmk_custom_setting_find(NULL, "blob/mode")) {
                LOG_ERR("Invalid write mode accepted arm=%u mode=%u ret=%d", (unsigned)tags[t],
                        invalid[m], ret);
                return -EINVAL;
            }
        }
    }
    return 0;
}
