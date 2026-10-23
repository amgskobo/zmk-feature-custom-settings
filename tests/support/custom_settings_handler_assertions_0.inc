#include "init_result.h"
/* SPDX-License-Identifier: MIT */
/* Test helper: pb_encode `setting` into buf, returning the byte count or -1
 * on failure. Setting.value's bytes_value/string_value is a callback field
 * (see the .options file), so there is no struct member a test can peek at
 * directly - a value only exists on the wire after a real pb_encode
 * (mirroring what the RPC transport does for GetSetting/ListSettings). */
static int test_encode_setting(const cormoran_zmk_custom_settings_Setting *setting, uint8_t *buf,
                               size_t buf_size) {
    pb_ostream_t stream = pb_ostream_from_buffer(buf, buf_size);
    if (!pb_encode(&stream, cormoran_zmk_custom_settings_Setting_fields, setting)) {
        LOG_ERR("test_encode_setting failed: %s", PB_GET_ERROR(&stream));
        return -1;
    }
    return (int)stream.bytes_written;
}

/* Test helper: pb_decode a Setting previously produced by test_encode_setting
 * and hand back the decoded bytes_value/string_value payload via `scratch`
 * (wired for decode before the call, same as production WriteSetting
 * decode). */
static int test_decode_setting(const uint8_t *buf, size_t buf_size,
                               cormoran_zmk_custom_settings_Setting *decoded,
                               struct bounded_decode_scratch *scratch) {
    *decoded = (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    wire_setting_value_decode(&decoded->value, scratch);
    pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
    if (!pb_decode(&stream, cormoran_zmk_custom_settings_Setting_fields, decoded)) {
        LOG_ERR("test_decode_setting failed: %s", PB_GET_ERROR(&stream));
        return -1;
    }
    return 0;
}

static int custom_settings_rpc_bytes_converter_test_init(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "bytes_value");
    if (!setting) {
        LOG_ERR("RPC bytes converter test setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    static cormoran_zmk_custom_settings_Setting proto_setting;
    proto_setting =
        (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(setting, &proto_setting, true, false, false,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }
    if (!proto_setting.has_value || proto_setting.value.which_value_type !=
                                        cormoran_zmk_custom_settings_SettingValue_bytes_value_tag) {
        LOG_ERR("RPC bytes converter test serialization failed");
        return -EINVAL;
    }

    /* The value only actually gets read/serialized when the streaming
     * callback fires during a real pb_encode - round-trip through the wire
     * to check what a client would actually receive. */
    static uint8_t encode_buf[128];
    int encoded = test_encode_setting(&proto_setting, encode_buf, sizeof(encode_buf));
    if (encoded < 0) {
        return -EIO;
    }
    cormoran_zmk_custom_settings_Setting decoded_setting;
    uint8_t decode_buf[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    struct bounded_decode_scratch decode_scratch = {
        .buf = decode_buf, .capacity = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE};
    if (test_decode_setting(encode_buf, encoded, &decoded_setting, &decode_scratch) < 0) {
        return -EIO;
    }
    if (decode_scratch.size != 3 || decode_scratch.buf[0] != 3 || decode_scratch.buf[1] != 2 ||
        decode_scratch.buf[2] != 1) {
        LOG_ERR("RPC bytes converter test serialization mismatch: size=%u %u,%u,%u",
                (unsigned)decode_scratch.size, decode_scratch.buf[0], decode_scratch.buf[1],
                decode_scratch.buf[2]);
        return -EINVAL;
    }

    struct zmk_custom_settings_setting_ref ref = {
        .has_custom_subsystem_id = true,
        .has_key = true,
        .has_source = true,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    };
    copy_string(ref.custom_subsystem_id, sizeof(ref.custom_subsystem_id), "test");
    copy_string(ref.key, sizeof(ref.key), "bytes_value");

    struct zmk_custom_setting_value rpc_value = ZMK_CUSTOM_SETTING_VALUE_BYTES(9, 8, 7);
    cormoran_zmk_custom_settings_Response resp = cormoran_zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_setting(
        &ref, &rpc_value, false, 0, 0,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp, true);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value internal_value;
    ret = zmk_custom_setting_read(setting, &internal_value);
    if (ret < 0) {
        return ret;
    }
    if (internal_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES || internal_value.size != 3 ||
        internal_value.bytes_value[0] != 7 || internal_value.bytes_value[1] != 8 ||
        internal_value.bytes_value[2] != 9) {
        LOG_ERR("RPC bytes converter test deserialization failed");
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_rpc_bytes_handler rpc=030201 internal=070809");
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_rpc_bytes_converter_test_init, APPLICATION, 99);

/* Setting.default_value is exposed only when the caller asks for it
 * (include_default) AND the current value differs from the default. Covers both
 * the inline scalar path (int) and the BYTES/STRING streaming-callback path
 * (string). */
static int custom_settings_default_value_test_init(void) {
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *string_setting =
        zmk_custom_setting_find("test", "string_value");
    if (!int_setting || !string_setting) {
        LOG_ERR("default value test settings not registered");
        return -ENOENT;
    }

    static cormoran_zmk_custom_settings_Setting proto_setting;
    int ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    /* At default, no default_value even when requested. */
    proto_setting =
        (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(int_setting, &proto_setting, true, false, true,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }
    if (proto_setting.has_default_value) {
        LOG_ERR("default value present when value equals default");
        return -EINVAL;
    }

    /* After changing it, the default_value carries the compile-time default. */
    struct zmk_custom_setting_value changed_int = ZMK_CUSTOM_SETTING_VALUE_INT32(55);
    ret = zmk_custom_setting_write(int_setting, &changed_int, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    proto_setting =
        (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(int_setting, &proto_setting, true, false, true,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }
    if (!proto_setting.has_default_value ||
        proto_setting.default_value.which_value_type !=
            cormoran_zmk_custom_settings_SettingValue_int32_value_tag ||
        proto_setting.default_value.value_type.int32_value != 10) {
        LOG_ERR("scalar default value not exposed correctly: has=%d which=%u val=%d",
                proto_setting.has_default_value,
                (unsigned)proto_setting.default_value.which_value_type,
                proto_setting.default_value.value_type.int32_value);
        return -EINVAL;
    }

    /* Not requested: absent even though the value now differs. */
    proto_setting =
        (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(int_setting, &proto_setting, true, false, false,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }
    if (proto_setting.has_default_value) {
        LOG_ERR("default value present when not requested");
        return -EINVAL;
    }
    ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    /* A STRING default streams through encode_setting_default_value: change the
     * value, encode a Setting with require_default, decode, and confirm the
     * default bytes are the compile-time default "hi". */
    ret = zmk_custom_setting_reset(string_setting);
    if (ret < 0) {
        return ret;
    }
    struct zmk_custom_setting_value changed_string = ZMK_CUSTOM_SETTING_VALUE_STRING("changed");
    ret = zmk_custom_setting_write(string_setting, &changed_string,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    proto_setting =
        (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(string_setting, &proto_setting, true, false, true,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }
    if (!proto_setting.has_default_value ||
        proto_setting.default_value.which_value_type !=
            cormoran_zmk_custom_settings_SettingValue_string_value_tag) {
        LOG_ERR("string default value arm not selected");
        return -EINVAL;
    }

    static uint8_t encode_buf[128];
    int encoded = test_encode_setting(&proto_setting, encode_buf, sizeof(encode_buf));
    if (encoded < 0) {
        return -EIO;
    }
    /* A bare Setting's value/default_value are plain submessage fields, so their
     * BYTES/STRING decode callbacks can be wired in advance (see the caution on
     * wire_setting_value_decode - the oneof-arm hazard only applies when a
     * SettingValue is reached across a parent oneof arm). Each needs its own
     * scratch since both are STRING here. */
    cormoran_zmk_custom_settings_Setting decoded = cormoran_zmk_custom_settings_Setting_init_zero;
    uint8_t value_buf[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    uint8_t default_buf[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    struct bounded_decode_scratch value_scratch = {
        .buf = value_buf, .capacity = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE};
    struct bounded_decode_scratch default_scratch = {
        .buf = default_buf, .capacity = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE};
    wire_setting_value_decode(&decoded.value, &value_scratch);
    wire_setting_value_decode(&decoded.default_value, &default_scratch);
    pb_istream_t stream = pb_istream_from_buffer(encode_buf, encoded);
    if (!pb_decode(&stream, cormoran_zmk_custom_settings_Setting_fields, &decoded)) {
        LOG_ERR("default value test decode failed: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    if (!decoded.has_default_value || default_scratch.size != 2 || default_buf[0] != 'h' ||
        default_buf[1] != 'i') {
        LOG_ERR("string default value payload mismatch: has=%d size=%u", decoded.has_default_value,
                (unsigned)default_scratch.size);
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(string_setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_default_value int=10 string=hi");
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_default_value_test_init, APPLICATION, 99);

/* ZMK_CUSTOM_SETTING_FOREACH yields exactly one registered zmk_custom_setting
 * per array (see for_each_list_item's comment); this regression-tests that
 * handle_private_list_settings still reports one counted item per *active
 * array element* (what the web UI/Studio protocol expect), not zero (which a
 * naive ZMK_CUSTOM_SETTING_FOREACH-only count would produce, since a bare
 * array descriptor's array_index is never < its own size) and not one
 * (counting the descriptor itself as a single list item). */
static int custom_settings_list_array_test_init(void) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array("test", "array_value");
    if (!array_setting) {
        LOG_ERR("List array test setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }
    uint32_t active_size = zmk_custom_setting_array_size(array_setting);
    if (active_size == 0) {
        LOG_ERR("List array test setting has no active elements after reset");
        return -EINVAL;
    }

    struct zmk_custom_settings_setting_scope scope = {
        .has_custom_subsystem_id = true,
        .has_key = true,
        .has_source = true,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    };
    copy_string(scope.custom_subsystem_id, sizeof(scope.custom_subsystem_id), "test");
    copy_string(scope.key, sizeof(scope.key), "array_value");

    uint32_t count = 0;
    for_each_list_item(&scope, list_count_visitor, &count);
    if (count != active_size) {
        LOG_ERR("List array test expected %u list items, got %u", active_size, count);
        return -EINVAL;
    }

    /* Send-side (list_send_visitor/for_each_list_item together) must walk
     * the same active_size flattened items, and each one must resolve to a
     * distinct, correctly-indexed element. */
    struct list_send_context ctx = {0};
    bool completed = for_each_list_item(&scope, list_send_visitor, &ctx);
    if (!completed || ctx.sent != active_size) {
        LOG_ERR("List array test send visited %u items (completed=%d), expected %u", ctx.sent,
                completed, active_size);
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_list_array_expands_to_elements count=%u", active_size);
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_list_array_test_init, APPLICATION, 99);

/* ListSettings/GetSetting must surface keyspace entries by their USER key
 * with their PAYLOAD value, even though a slot is stored internally as an
 * anonymous [key\0][payload] BYTES blob under an ordinal storage name.
 * Exercises for_each_list_item's explicit keyspace-slot pass plus the full
 * setting_to_proto presentation path. Uses the "test"/"macro/" keyspace
 * registered by custom_settings_test.c (reached through the compile-time
 * keyspace section, so no cross-file symbol is needed). */
static int custom_settings_list_keyspace_test_init(void) {
    struct zmk_custom_setting_keyspace *keyspace =
        zmk_custom_settings_keyspace_find_for_key("test", "macro/list-test");
    if (!keyspace) {
        LOG_ERR("List keyspace test: no keyspace registered for test/macro/");
        return -ENOENT;
    }

    const struct zmk_custom_setting *created = NULL;
    struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
        .int32_value = 42,
    };
    int ret = zmk_custom_setting_keyspace_create(keyspace, "macro/list-test", &value,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &created);
    if (ret < 0 || !created) {
        LOG_ERR("List keyspace test: create failed: %d", ret);
        return ret < 0 ? ret : -EINVAL;
    }

    struct zmk_custom_settings_setting_scope scope = {
        .has_custom_subsystem_id = true,
        .has_key_prefix = true,
        .has_source = true,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    };
    copy_string(scope.custom_subsystem_id, sizeof(scope.custom_subsystem_id), "test");
    copy_string(scope.key_prefix, sizeof(scope.key_prefix), "macro/");

    uint32_t count = 0;
    for_each_list_item(&scope, list_count_visitor, &count);
    if (count != 1) {
        LOG_ERR("List keyspace test: expected 1 list item under macro/, got %u", count);
        return -EINVAL;
    }

    /* The presented Setting must carry the USER key and the typed payload,
     * not the ordinal storage name / raw blob. */
    cormoran_zmk_custom_settings_Setting proto_setting =
        cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(created, &proto_setting, true, false, false,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        LOG_ERR("List keyspace test: setting_to_proto failed: %d", ret);
        return ret;
    }
    if (strcmp(proto_setting.key, "macro/list-test") != 0) {
        LOG_ERR("List keyspace test: expected user key, got \"%s\"", proto_setting.key);
        return -EINVAL;
    }
    if (!proto_setting.has_value ||
        proto_setting.value.which_value_type !=
            cormoran_zmk_custom_settings_SettingValue_int32_value_tag ||
        proto_setting.value.value_type.int32_value != 42) {
        LOG_ERR("List keyspace test: expected int32 payload 42 (which=%u)",
                (unsigned)proto_setting.value.which_value_type);
        return -EINVAL;
    }

    ret = zmk_custom_setting_keyspace_delete(keyspace, "macro/list-test");
    if (ret < 0) {
        LOG_ERR("List keyspace test: delete failed: %d", ret);
        return ret;
    }
    count = 0;
    for_each_list_item(&scope, list_count_visitor, &count);
    if (count != 0) {
        LOG_ERR("List keyspace test: expected 0 list items after delete, got %u", count);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_list_keyspace_user_key key=macro/list-test value=42");
    return 0;
}

/* Priority 99 like the other self-tests. Link order relative to
 * custom_settings_test.c's SYS_INIT (same level) does not matter: this test
 * only needs the keyspace section entry (compile-time) and an empty "macro/"
 * prefix, and the lifecycle test deletes every entry it creates before
 * returning. */
OWNER_CHECKED_SYS_INIT(custom_settings_list_keyspace_test_init, APPLICATION, 99);
