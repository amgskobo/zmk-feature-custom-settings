#include "init_result.h"
/* SPDX-License-Identifier: MIT */
#include "chunk_identity_cases.inc"

static int custom_settings_chunked_rpc_test_init(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "bytes_value");
    if (!setting) {
        LOG_ERR("Chunked RPC test setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_settings_setting_ref ref = {
        .has_custom_subsystem_id = true,
        .has_key = true,
        .has_source = true,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    };
    copy_string(ref.custom_subsystem_id, sizeof(ref.custom_subsystem_id), "test");
    copy_string(ref.key, sizeof(ref.key), "bytes_value");

    /* Write RPC bytes [9, 8, 7] (-> internal [7, 8, 9] via the reversing
     * converter) across two chunks. */
    uint8_t chunk1[] = {9, 8};
    cormoran_zmk_custom_settings_Response resp = cormoran_zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_value_chunk(
        &ref, 3, 0, chunk1, sizeof(chunk1), false,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp);
    if (ret < 0) {
        return ret;
    }
    if (resp.which_response_type != cormoran_zmk_custom_settings_Response_status_tag ||
        resp.response_type.status.affected_count != 2) {
        LOG_ERR("Chunked RPC write did not report bytes received so far");
        return -EINVAL;
    }

    uint8_t chunk2[] = {7};
    resp = (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_value_chunk(
        &ref, 3, 2, chunk2, sizeof(chunk2), true,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp);
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
        LOG_ERR("Chunked RPC write did not assemble the expected value");
        return -EINVAL;
    }

    /* Out-of-order chunk (wrong offset) must be rejected and must not
     * disturb the value written above. */
    uint8_t bad_chunk[] = {1};
    resp = (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_value_chunk(
        &ref, 3, 1, bad_chunk, sizeof(bad_chunk), false,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp);
    if (ret != -EINVAL) {
        LOG_ERR("Expected out-of-order chunk write to be rejected, got %d", ret);
        return -EINVAL;
    }

    /* Read the value back the plain way (there is no ReadValueChunk):
     * GetSetting's Setting.value streams the assembled value through the same
     * callback path as any other BYTES/STRING setting - round-trip it through
     * a real pb_encode/pb_decode and check the RPC-serialized bytes the client
     * would actually see. */
    static cormoran_zmk_custom_settings_Setting proto_setting;
    proto_setting =
        (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(setting, &proto_setting, true, false, false,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }
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
    if (decode_scratch.size != 3 || decode_scratch.buf[0] != 9 || decode_scratch.buf[1] != 8 ||
        decode_scratch.buf[2] != 7) {
        LOG_ERR("Chunked RPC write did not read back the expected value via GetSetting");
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    int identity_result = custom_settings_chunk_identity_case();
    if (identity_result) {
        return identity_result;
    }

    LOG_INF("PASS: custom_settings_chunked_rpc rpc=030201->090807 internal=070809");
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_chunked_rpc_test_init, APPLICATION, 99);

/* The Studio stream is paced by the host, so a write to it can wait for as long
 * as the host takes. This stream notes, at each write, whether the writing
 * thread still owns Zephyr's settings lock or the module's state mutex. */
extern struct k_mutex settings_lock;
extern struct k_mutex custom_settings_lock;

struct lock_probe {
    unsigned int writes;
    bool held;
};

static bool mutex_owned_by_current(const struct k_mutex *mutex) {
    return mutex->lock_count != 0U && mutex->owner == k_current_get();
}

static bool lock_probe_write(pb_ostream_t *stream, const pb_byte_t *buf, size_t count) {
    ARG_UNUSED(buf);
    ARG_UNUSED(count);
    struct lock_probe *probe = stream->state;

    probe->writes++;
    if (mutex_owned_by_current(&settings_lock) || mutex_owned_by_current(&custom_settings_lock)) {
        probe->held = true;
    }
    return true;
}

/* A value larger than the fixed carrier (and larger than one chunk frame)
 * must round-trip over WriteValueChunk into a large-sized setting's backing
 * buffer, and then back out through the plain (non-chunked) GetSetting encode
 * path - the streaming callback is what makes that possible without
 * ReadValueChunk. */
static int custom_settings_large_chunked_rpc_test_init(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "large_bytes");
    if (!setting) {
        LOG_ERR("Large chunked RPC test setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_settings_setting_ref ref = {
        .has_custom_subsystem_id = true,
        .has_key = true,
        .has_source = true,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    };
    copy_string(ref.custom_subsystem_id, sizeof(ref.custom_subsystem_id), "test");
    copy_string(ref.key, sizeof(ref.key), "large_bytes");

    const uint32_t total = 200;
    uint8_t payload[200];
    for (uint32_t i = 0; i < total; i++) {
        payload[i] = (uint8_t)(i * 3 + 1);
    }

    /* Write across two > single-frame-limited chunks (128 + 72). */
    cormoran_zmk_custom_settings_Response resp = cormoran_zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_value_chunk(
        &ref, total, 0, payload, 128, false,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp);
    if (ret < 0) {
        return ret;
    }
    resp = (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_value_chunk(
        &ref, total, 128, payload + 128, total - 128, true,
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp);
    if (ret < 0) {
        return ret;
    }

    uint8_t readback[200];
    size_t out_size = 0;
    ret = zmk_custom_setting_read_into(setting, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != total || memcmp(readback, payload, total) != 0) {
        LOG_ERR("Large chunked write did not assemble the expected value: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    /* Read it back the plain way (there is no ReadValueChunk): a plain
     * GetSetting-style pb_encode into a native_sim buffer large enough for the
     * whole value, streamed through encode_setting_scalar_value's large-store
     * path (no CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE cap), then decoded
     * back and compared byte-exact against the original payload. */
    static cormoran_zmk_custom_settings_Setting proto_setting;
    proto_setting =
        (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(setting, &proto_setting, true, false, false,
                           ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }

    static uint8_t large_encode_buf[512];
    int encoded = test_encode_setting(&proto_setting, large_encode_buf, sizeof(large_encode_buf));
    if (encoded < 0) {
        return -EIO;
    }

    static cormoran_zmk_custom_settings_Setting decoded_setting;
    static uint8_t large_decode_buf[512];
    static struct bounded_decode_scratch large_decode_scratch;
    large_decode_scratch =
        (struct bounded_decode_scratch){.buf = large_decode_buf, .capacity = sizeof(readback)};
    if (test_decode_setting(large_encode_buf, encoded, &decoded_setting, &large_decode_scratch) <
        0) {
        return -EIO;
    }
    if (large_decode_scratch.size != total ||
        memcmp(large_decode_scratch.buf, payload, total) != 0) {
        LOG_ERR("Large value did not round-trip through plain GetSetting encode: size=%u",
                (unsigned)large_decode_scratch.size);
        return -EINVAL;
    }

    /* The same encode into a stream that watches the locks. The value above is
     * larger than the fixed carrier, so this goes through the large-value path:
     * it must copy the value out and write to the stream with nothing held. */
    struct lock_probe probe = {0};
    pb_ostream_t probe_stream = {&lock_probe_write, &probe, SIZE_MAX, 0};
    if (!pb_encode(&probe_stream, cormoran_zmk_custom_settings_Setting_fields, &proto_setting) ||
        probe.writes == 0U) {
        LOG_ERR("Large value probe encode failed: writes=%u", probe.writes);
        return -EIO;
    }
    if (probe.held) {
        LOG_ERR("Large value encode kept a settings lock across a stream write");
        return -EBUSY;
    }

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_large_chunked_rpc size=%u", (unsigned)total);
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_large_chunked_rpc_test_init, APPLICATION, 99);
