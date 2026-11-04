#include "init_result.h"
/* SPDX-License-Identifier: MIT */
/*
 * Regression test for nanopb oneof-submessage callback lifetime: a
 * SettingValue payload callback wired BEFORE pb_decode()
 * into Request.write_setting/.create_setting is zeroed when nanopb selects
 * the oneof arm, so the STRING/BYTES payload was silently skipped and
 * which_value_type stayed 0 ("Invalid request"). This decodes full
 * wire-format Requests through decode_custom_settings_request() - the exact
 * production entry path - and asserts the payload reaches the scratch. The
 * pre-fix code passes every other native test but fails here.
 */
#include "rpc_write_mode_cases.inc"
#include "scope_permission_cases.inc"
#include "review_rpc_cases.inc"

static int custom_settings_request_decode_regression_test_init(void) {
    static uint8_t enc_buf[64];
    static struct bounded_decode_scratch enc_scratch = {.buf = enc_buf,
                                                        .capacity = sizeof(enc_buf) - 1};
    static const char payload[] = "callback-ok";

    /* --- write_setting arm with a STRING payload --- */
    enc_scratch.size = sizeof(payload) - 1;
    memcpy(enc_scratch.buf, payload, enc_scratch.size);

    cormoran_zmk_custom_settings_Request req = cormoran_zmk_custom_settings_Request_init_zero;
    req.which_request_type = cormoran_zmk_custom_settings_Request_write_setting_tag;
    req.request_type.write_setting.has_setting = true;
    req.request_type.write_setting.setting.has_key = true;
    copy_string(req.request_type.write_setting.setting.key,
                sizeof(req.request_type.write_setting.setting.key), "string_value");
    req.request_type.write_setting.has_value = true;
    req.request_type.write_setting.value.which_value_type =
        cormoran_zmk_custom_settings_SettingValue_string_value_tag;
    req.request_type.write_setting.value.value_type.string_value.funcs.encode =
        encode_bounded_scratch_value;
    req.request_type.write_setting.value.value_type.string_value.arg = &enc_scratch;

    uint8_t wire[128];
    pb_ostream_t ostream = pb_ostream_from_buffer(wire, sizeof(wire));
    if (!pb_encode(&ostream, cormoran_zmk_custom_settings_Request_fields, &req)) {
        LOG_ERR("request decode regression: encode failed: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }

    cormoran_zmk_custom_settings_Request decoded;
    if (!decode_custom_settings_request(wire, ostream.bytes_written, &decoded)) {
        LOG_ERR("request decode regression: decode failed");
        return -EIO;
    }
    if (decoded.which_request_type != cormoran_zmk_custom_settings_Request_write_setting_tag ||
        decoded.request_type.write_setting.value.which_value_type !=
            cormoran_zmk_custom_settings_SettingValue_string_value_tag) {
        LOG_ERR("request decode regression: write_setting arm/value tag lost (%u/%u)",
                decoded.which_request_type,
                decoded.request_type.write_setting.value.which_value_type);
        return -EINVAL;
    }
    if (g_write_value_decode_scratch.size != sizeof(payload) - 1 ||
        memcmp(g_write_value_decode_scratch.buf, payload, sizeof(payload) - 1) != 0) {
        LOG_ERR("request decode regression: write_setting payload lost (size=%u)",
                (unsigned)g_write_value_decode_scratch.size);
        return -EINVAL;
    }

    /* --- create_setting arm with a BYTES payload --- */
    static const uint8_t blob_payload[] = {0xA5, 0x5A, 0x11, 0x22};
    enc_scratch.size = sizeof(blob_payload);
    memcpy(enc_scratch.buf, blob_payload, enc_scratch.size);

    req = (cormoran_zmk_custom_settings_Request)cormoran_zmk_custom_settings_Request_init_zero;
    req.which_request_type = cormoran_zmk_custom_settings_Request_create_setting_tag;
    req.request_type.create_setting.has_setting = true;
    req.request_type.create_setting.setting.has_key = true;
    copy_string(req.request_type.create_setting.setting.key,
                sizeof(req.request_type.create_setting.setting.key), "blob/regress");
    req.request_type.create_setting.has_value = true;
    req.request_type.create_setting.value.which_value_type =
        cormoran_zmk_custom_settings_SettingValue_bytes_value_tag;
    req.request_type.create_setting.value.value_type.bytes_value.funcs.encode =
        encode_bounded_scratch_value;
    req.request_type.create_setting.value.value_type.bytes_value.arg = &enc_scratch;

    ostream = pb_ostream_from_buffer(wire, sizeof(wire));
    if (!pb_encode(&ostream, cormoran_zmk_custom_settings_Request_fields, &req)) {
        LOG_ERR("request decode regression: create encode failed: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    if (!decode_custom_settings_request(wire, ostream.bytes_written, &decoded)) {
        LOG_ERR("request decode regression: create decode failed");
        return -EIO;
    }
    if (decoded.which_request_type != cormoran_zmk_custom_settings_Request_create_setting_tag ||
        decoded.request_type.create_setting.value.which_value_type !=
            cormoran_zmk_custom_settings_SettingValue_bytes_value_tag ||
        g_write_value_decode_scratch.size != sizeof(blob_payload) ||
        memcmp(g_write_value_decode_scratch.buf, blob_payload, sizeof(blob_payload)) != 0) {
        LOG_ERR("request decode regression: create_setting payload lost (size=%u)",
                (unsigned)g_write_value_decode_scratch.size);
        return -EINVAL;
    }

    int invalid_mode_result = custom_settings_invalid_write_modes();
    if (invalid_mode_result) {
        return invalid_mode_result;
    }
    int permission_result = custom_settings_scope_permission_cases();
    if (permission_result) {
        return permission_result;
    }
    int review_result = custom_settings_review_rpc_cases();
    if (review_result) {
        return review_result;
    }

    LOG_INF("PASS: custom_settings_request_decode_regression write=%s create=%u bytes", payload,
            (unsigned)sizeof(blob_payload));
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_request_decode_regression_test_init, APPLICATION, 99);

/*
 * Regression test for a oneof-callback lifetime hazard: a Notification DECODED
 * with the message-level
 * precallback wired must have cb_notification_type cleared before it is
 * RE-ENCODED (the central's relayed-notification republish path,
 * relayed_notification_to_public). nanopb's encoder invokes
 * cb_<oneof>.funcs.encode for SUBMSG_W_CB arms, and after a struct copy the
 * funcs union still holds the decode precallback - which then runs in
 * encode context and overwrites the value union with the decode callback's
 * pointers. A copied decode callback must not run as an encode callback:
 * otherwise an INT32 can become a callback address and BYTES/STRING data can
 * be lost. This mirrors the decode -> sanitize -> re-encode sequence and
 * asserts both an INT32 and a STRING value survive the round trip.
 */
static int custom_settings_notification_reencode_regression_test_init(void) {
    static uint8_t value_buf[64];
    static struct bounded_decode_scratch value_scratch = {.buf = value_buf,
                                                          .capacity = sizeof(value_buf) - 1};
    static uint8_t payload_buf[64];
    static struct bounded_decode_scratch payload_scratch = {.buf = payload_buf,
                                                            .capacity = sizeof(payload_buf) - 1};
    static const char str_payload[] = "relay-p4";
    uint8_t wire[160];
    uint8_t wire2[160];

    for (int use_string = 0; use_string <= 1; use_string++) {
        /* 1. Encode a source notification (as a peripheral would). */
        cormoran_zmk_custom_settings_Notification src =
            cormoran_zmk_custom_settings_Notification_init_zero;
        src.which_notification_type = cormoran_zmk_custom_settings_Notification_setting_tag;
        src.notification_type.setting.kind =
            cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_VALUE_UPDATED;
        src.notification_type.setting.has_setting = true;
        copy_string(src.notification_type.setting.setting.key,
                    sizeof(src.notification_type.setting.setting.key), "int32_value");
        src.notification_type.setting.setting.has_value = true;
        if (use_string) {
            payload_scratch.size = sizeof(str_payload) - 1;
            memcpy(payload_scratch.buf, str_payload, payload_scratch.size);
            src.notification_type.setting.setting.value.which_value_type =
                cormoran_zmk_custom_settings_SettingValue_string_value_tag;
            src.notification_type.setting.setting.value.value_type.string_value.funcs.encode =
                encode_bounded_scratch_value;
            src.notification_type.setting.setting.value.value_type.string_value.arg =
                &payload_scratch;
        } else {
            src.notification_type.setting.setting.value.which_value_type =
                cormoran_zmk_custom_settings_SettingValue_int32_value_tag;
            src.notification_type.setting.setting.value.value_type.int32_value = 63;
        }
        pb_ostream_t out = pb_ostream_from_buffer(wire, sizeof(wire));
        if (!pb_encode(&out, cormoran_zmk_custom_settings_Notification_fields, &src)) {
            LOG_ERR("notification reencode regression: encode failed: %s", PB_GET_ERROR(&out));
            return -EIO;
        }

        /* 2. Decode it the way relayed_notification_to_public does. */
        cormoran_zmk_custom_settings_Notification decoded =
            cormoran_zmk_custom_settings_Notification_init_zero;
        value_scratch.size = 0;
        decoded.cb_notification_type.funcs.decode = notification_arm_wire_precallback;
        decoded.cb_notification_type.arg = &value_scratch;
        pb_istream_t in = pb_istream_from_buffer(wire, out.bytes_written);
        if (!pb_decode(&in, cormoran_zmk_custom_settings_Notification_fields, &decoded)) {
            LOG_ERR("notification reencode regression: decode failed: %s", PB_GET_ERROR(&in));
            return -EIO;
        }

        /* 3. Sanitize + retarget exactly like the republish path, then
         * re-encode. Without the cb clear this second encode corrupts the
         * value. */
        cormoran_zmk_custom_settings_Notification republish = decoded;
        republish.cb_notification_type = (pb_callback_t){0};
        if (republish.notification_type.setting.setting.has_value) {
            retarget_value_to_encode_scratch(&republish.notification_type.setting.setting.value,
                                             &value_scratch);
        }
        out = pb_ostream_from_buffer(wire2, sizeof(wire2));
        if (!pb_encode(&out, cormoran_zmk_custom_settings_Notification_fields, &republish)) {
            LOG_ERR("notification reencode regression: re-encode failed: %s", PB_GET_ERROR(&out));
            return -EIO;
        }

        /* 4. Decode the re-encoded wire and check the value survived. */
        cormoran_zmk_custom_settings_Notification final_msg =
            cormoran_zmk_custom_settings_Notification_init_zero;
        static uint8_t final_buf[64];
        static struct bounded_decode_scratch final_scratch = {.buf = final_buf,
                                                              .capacity = sizeof(final_buf) - 1};
        final_scratch.size = 0;
        final_msg.cb_notification_type.funcs.decode = notification_arm_wire_precallback;
        final_msg.cb_notification_type.arg = &final_scratch;
        in = pb_istream_from_buffer(wire2, out.bytes_written);
        if (!pb_decode(&in, cormoran_zmk_custom_settings_Notification_fields, &final_msg)) {
            LOG_ERR("notification reencode regression: final decode failed: %s", PB_GET_ERROR(&in));
            return -EIO;
        }
        const cormoran_zmk_custom_settings_SettingValue *v =
            &final_msg.notification_type.setting.setting.value;
        if (use_string) {
            if (v->which_value_type != cormoran_zmk_custom_settings_SettingValue_string_value_tag ||
                final_scratch.size != sizeof(str_payload) - 1 ||
                memcmp(final_scratch.buf, str_payload, final_scratch.size) != 0) {
                LOG_ERR("notification reencode regression: STRING lost (which=%u size=%u)",
                        v->which_value_type, (unsigned)final_scratch.size);
                return -EINVAL;
            }
        } else {
            if (v->which_value_type != cormoran_zmk_custom_settings_SettingValue_int32_value_tag ||
                v->value_type.int32_value != 63) {
                LOG_ERR("notification reencode regression: INT32 lost (which=%u val=%d)",
                        v->which_value_type, v->value_type.int32_value);
                return -EINVAL;
            }
        }
    }

    LOG_INF("PASS: custom_settings_notification_reencode_regression int32=63 string=%s",
            str_payload);
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_notification_reencode_regression_test_init, APPLICATION, 99);

/*
 * Regression test for RPC response/notification ordering: a mutating Studio
 * RPC must not queue a redundant notification that can compete with its
 * still-in-flight response on a single-connection transport. The fix
 * brackets RPC dispatch with notify_suppress_begin/end so an RPC-originated
 * mutation queues ZERO notifications, while non-RPC mutations keep notifying.
 *
 * Step A drives a direct (non-RPC) write and asserts the notification-submit
 * counter increments. Step B drives the SAME setting through the real RPC
 * entry point (custom_settings_rpc_handle_request) and asserts the counter
 * stays 0. Removing the notify_suppressed() guard makes Step B fail.
 */
static int custom_settings_rpc_suppresses_notification_test_init(void) {
    const char *subsys = "test";
    const char *key = "int_value";

    uint32_t subsystem_index = 0;
    if (custom_subsystem_index_for_identifier(subsys, &subsystem_index) < 0) {
        LOG_ERR("rpc suppression regression: subsystem %s not registered", subsys);
        return -ENOENT;
    }

    /* Step A: a direct (non-RPC) mutation must still submit a notification. */
    notify_submit_count_for_test = 0;
    struct zmk_custom_setting_value a_value = ZMK_CUSTOM_SETTING_VALUE_INT32(11);
    int ret = zmk_custom_setting_write_by_key(subsys, key, &a_value,
                                              ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("rpc suppression regression: non-RPC write failed: %d", ret);
        return ret;
    }
    uint32_t nonrpc_count = notify_submit_count_for_test;
    if (nonrpc_count == 0) {
        LOG_ERR("rpc suppression regression: non-RPC write submitted no notification");
        return -EINVAL;
    }

    /* Step B: the SAME write through the real RPC entry must submit none. Build
     * a wire-encoded write_setting Request exactly as Studio would send it. */
    cormoran_zmk_custom_settings_Request req = cormoran_zmk_custom_settings_Request_init_zero;
    req.which_request_type = cormoran_zmk_custom_settings_Request_write_setting_tag;
    req.request_type.write_setting.has_setting = true;
    req.request_type.write_setting.setting.has_custom_subsystem_index = true;
    req.request_type.write_setting.setting.custom_subsystem_index = subsystem_index;
    req.request_type.write_setting.setting.has_key = true;
    copy_string(req.request_type.write_setting.setting.key,
                sizeof(req.request_type.write_setting.setting.key), key);
    req.request_type.write_setting.has_value = true;
    req.request_type.write_setting.value.which_value_type =
        cormoran_zmk_custom_settings_SettingValue_int32_value_tag;
    req.request_type.write_setting.value.value_type.int32_value = 22;
    req.request_type.write_setting.mode =
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY;

    static uint8_t wire[96];
    pb_ostream_t ostream = pb_ostream_from_buffer(wire, sizeof(wire));
    if (!pb_encode(&ostream, cormoran_zmk_custom_settings_Request_fields, &req)) {
        LOG_ERR("rpc suppression regression: encode failed: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }

    zmk_custom_CallRequest call = zmk_custom_CallRequest_init_zero;
    if (ostream.bytes_written > sizeof(call.payload.bytes)) {
        LOG_ERR("rpc suppression regression: encoded request too large (%u)",
                (unsigned)ostream.bytes_written);
        return -ENOMEM;
    }
    memcpy(call.payload.bytes, wire, ostream.bytes_written);
    call.payload.size = ostream.bytes_written;

    /* Drive the real production entry point so the notify_suppress bracket is
     * exercised end to end. */
    notify_submit_count_for_test = 0;
    pb_callback_t encode_response = {0};
    if (!custom_settings_rpc_handle_request(&call, &encode_response)) {
        LOG_ERR("rpc suppression regression: RPC handler returned false");
        return -EIO;
    }
    uint32_t rpc_count = notify_submit_count_for_test;
    if (rpc_count != 0) {
        LOG_ERR("rpc suppression regression: RPC write submitted %u notification(s)", rpc_count);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_rpc_suppresses_notification nonrpc=%u rpc=%u", nonrpc_count,
            rpc_count);
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_rpc_suppresses_notification_test_init, APPLICATION, 99);

/*
 * The PUBLIC suppress API (zmk_custom_settings_notify_suppress_begin/end) lets
 * another module suppress the self-notification of a core write it drives from
 * its own Studio RPC handler. A direct write inside the bracket must submit no
 * notification; the same write outside it must submit one. (This is what
 * zmk-feature-runtime-macro uses so its CreateMacro/etc. RPCs do not fire a
 * redundant custom-settings notification that would starve the RPC response
 * and overflow the RPC thread stack.)
 */
/* Call-scoped quiet events must reach core consumers without suppressing an
 * independent nested writer. The real Studio listener counts queue submissions. */
static const struct zmk_custom_setting *quiet_observed_setting;
static const struct zmk_custom_setting *quiet_nested_setting;
static uint32_t quiet_core_events;
static bool quiet_observed_flag;
static enum zmk_custom_setting_changed_kind quiet_observed_kind;
static int quiet_nested_result;
static int quiet_event_observer(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *event = as_zmk_custom_setting_changed(eh);
    if (event != NULL && event->setting == quiet_observed_setting) {
        quiet_core_events++;
        quiet_observed_flag = event->suppress_studio_notification;
        quiet_observed_kind = event->kind;
        if (quiet_nested_setting != NULL) {
            const struct zmk_custom_setting *nested = quiet_nested_setting;
            quiet_nested_setting = NULL;
            quiet_nested_result = zmk_custom_setting_write_bytes(
                nested, "abc", 3, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(custom_settings_quiet_observer, quiet_event_observer);
ZMK_SUBSCRIPTION(custom_settings_quiet_observer, zmk_custom_setting_changed);

static void quiet_expect_begin(const struct zmk_custom_setting *setting) {
    quiet_observed_setting = setting;
    quiet_core_events = 0;
    quiet_observed_flag = false;
    notify_submit_count_for_test = 0;
}
static int quiet_expect_end(int ret, bool quiet, enum zmk_custom_setting_changed_kind kind) {
    if (ret != 0 || quiet_core_events != 1 || quiet_observed_flag != quiet ||
        quiet_observed_kind != kind ||
        (quiet ? notify_submit_count_for_test != 0 : notify_submit_count_for_test != 1)) {
        LOG_ERR("quiet regression: ret=%d events=%u flag=%d notify=%u kind=%d", ret,
                quiet_core_events, quiet_observed_flag, notify_submit_count_for_test,
                quiet_observed_kind);
        return -EINVAL;
    }
    return 0;
}
#define QUIET_EXPECT(setting, quiet, kind, expression)                                             \
    do {                                                                                           \
        quiet_expect_begin(setting);                                                               \
        int result = quiet_expect_end((expression), quiet, kind);                                  \
        if (result != 0)                                                                           \
            return result;                                                                         \
    } while (0)

static int quiet_expect_bytes(const struct zmk_custom_setting *setting, const uint8_t *expected,
                              size_t expected_size) {
    uint8_t actual[100];
    size_t size = 0;
    enum zmk_custom_setting_value_type type;
    int ret = zmk_custom_setting_read_into(setting, actual, sizeof(actual), &size, &type);
    return ret == 0 && type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES && size == expected_size &&
                   memcmp(actual, expected, size) == 0
               ? 0
               : -EINVAL;
}

static int custom_settings_quiet_operations(void) {
    const struct zmk_custom_setting *scalar = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *bytes = zmk_custom_setting_find("test", "bytes_value");
    const struct zmk_custom_setting *large = zmk_custom_setting_find("test", "large_bytes");
    if (scalar == NULL || bytes == NULL || large == NULL)
        return -ENOENT;
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(67);
    QUIET_EXPECT(
        scalar, true, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
        zmk_custom_setting_write_quiet(scalar, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    if (zmk_custom_setting_read(scalar, &value) != 0 || value.int32_value != 67)
        return -EINVAL;
    value = ZMK_CUSTOM_SETTING_VALUE_INT32(80);
    QUIET_EXPECT(scalar, false, ZMK_CUSTOM_SETTING_CHANGED_SAVED,
                 zmk_custom_setting_write(scalar, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST));
    QUIET_EXPECT(scalar, true, ZMK_CUSTOM_SETTING_CHANGED_SAVED,
                 zmk_custom_setting_save_quiet(scalar));
    if (zmk_custom_setting_read(scalar, &value) != 0 || value.int32_value != 80)
        return -EINVAL;
    QUIET_EXPECT(scalar, false, ZMK_CUSTOM_SETTING_CHANGED_SAVED, zmk_custom_setting_save(scalar));
    const uint8_t payload[] = {7, 8, 9};
    QUIET_EXPECT(bytes, true, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                 zmk_custom_setting_write_bytes_quiet(bytes, payload, sizeof(payload),
                                                      ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    if (quiet_expect_bytes(bytes, payload, sizeof(payload)) != 0)
        return -EINVAL;
    QUIET_EXPECT(bytes, false, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                 zmk_custom_setting_write_bytes(bytes, payload, sizeof(payload),
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    uint8_t raw[100];
    memset(raw, 0x5a, sizeof(raw));
    QUIET_EXPECT(large, true, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                 zmk_custom_setting_write_bytes_quiet(large, raw, sizeof(raw),
                                                      ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    if (quiet_expect_bytes(large, raw, sizeof(raw)) != 0)
        return -EINVAL;
    QUIET_EXPECT(large, false, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                 zmk_custom_setting_write_bytes(large, raw, sizeof(raw),
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    struct zmk_custom_setting_keyspace *keyspace =
        zmk_custom_settings_keyspace_find_for_key("test", "blob/quiet");
    const struct zmk_custom_setting *entry = NULL;
    value = ZMK_CUSTOM_SETTING_VALUE_BYTES(1, 2, 3);
    int ret = zmk_custom_setting_keyspace_create(keyspace, "blob/quiet", &value,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &entry);
    if (ret != 0 || entry == NULL)
        return ret != 0 ? ret : -EINVAL;
    QUIET_EXPECT(
        entry, true, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
        zmk_custom_setting_write_quiet(entry, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    const uint8_t initial_payload[] = {1, 2, 3};
    if (quiet_expect_bytes(entry, initial_payload, sizeof(initial_payload)) != 0)
        return -EINVAL;
    QUIET_EXPECT(entry, false, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                 zmk_custom_setting_write(entry, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    QUIET_EXPECT(entry, true, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                 zmk_custom_setting_write_bytes_quiet(entry, raw, sizeof(raw),
                                                      ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    if (quiet_expect_bytes(entry, raw, sizeof(raw)) != 0)
        return -EINVAL;
    QUIET_EXPECT(entry, false, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                 zmk_custom_setting_write_bytes(entry, raw, sizeof(raw),
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY));
    ret = zmk_custom_setting_keyspace_delete(keyspace, "blob/quiet");
    if (ret != 0)
        return ret;
    /* Quiet is carried by one event, not a global flag: this independent
     * ordinary mutation from inside a quiet event must still notify. */
    quiet_expect_begin(scalar);
    quiet_nested_setting = bytes;
    quiet_nested_result = -EINPROGRESS;
    value = ZMK_CUSTOM_SETTING_VALUE_INT32(44);
    ret = zmk_custom_setting_write_quiet(scalar, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != 0 || quiet_core_events != 1 || !quiet_observed_flag || quiet_nested_result != 0 ||
        quiet_nested_setting != NULL || notify_submit_count_for_test != 1)
        return -EINVAL;
    quiet_observed_setting = NULL;
    return 0;
}
#undef QUIET_EXPECT

static int custom_settings_public_suppress_test_init(void) {
    const char *subsys = "test";
    const char *key = "int_value";
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(33);

    notify_submit_count_for_test = 0;
    zmk_custom_settings_notify_suppress_begin();
    int ret =
        zmk_custom_setting_write_by_key(subsys, key, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    zmk_custom_settings_notify_suppress_end();
    if (ret < 0) {
        LOG_ERR("public suppress regression: bracketed write failed: %d", ret);
        return ret;
    }
    uint32_t suppressed_count = notify_submit_count_for_test;
    if (suppressed_count != 0) {
        LOG_ERR("public suppress regression: bracketed write submitted %u notification(s)",
                suppressed_count);
        return -EINVAL;
    }

    /* After end(), the depth is back to 0, so a plain write notifies again. */
    notify_submit_count_for_test = 0;
    value = ZMK_CUSTOM_SETTING_VALUE_INT32(44);
    ret =
        zmk_custom_setting_write_by_key(subsys, key, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    if (notify_submit_count_for_test == 0) {
        LOG_ERR("public suppress regression: write after end() submitted no notification");
        return -EINVAL;
    }

    uint32_t after_count = notify_submit_count_for_test;
    ret = custom_settings_quiet_operations();
    if (ret != 0)
        return ret;

    LOG_INF("PASS: custom_settings_public_suppress suppressed=%u after=%u", suppressed_count,
            after_count);
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_public_suppress_test_init, APPLICATION, 99);

/*
 * ZMK's official factory reset (Studio core `reset_settings` RPC) must wipe
 * custom settings via our registered ZMK_RPC_SUBSYSTEM_SETTINGS_RESET callback.
 * Persist a non-default value, drive the SAME iterator ZMK's core handler uses
 * (ZMK_RPC_SUBSYSTEM_SETTINGS_RESET_FOREACH), and assert our callback ran and
 * reverted the setting to its default with no unsaved state left.
 */
static int custom_settings_reset_all_test_init(void) {
    const char *subsys = "test";
    const char *key = "int_value";
    const int32_t default_value = 10;

    const struct zmk_custom_setting *setting = zmk_custom_setting_find(subsys, key);
    if (!setting) {
        LOG_ERR("reset_all regression: test/int_value not found");
        return -ENOENT;
    }

    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(66);
    int ret =
        zmk_custom_setting_write_by_key(subsys, key, &value, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_ERR("reset_all regression: persist failed: %d", ret);
        return ret;
    }

    /* Invoke the official reset exactly as ZMK's core subsystem does. */
    int callbacks = 0;
    ZMK_RPC_SUBSYSTEM_SETTINGS_RESET_FOREACH(sub) {
        callbacks++;
        ret = sub->callback();
        if (ret < 0) {
            LOG_ERR("reset_all regression: reset callback failed: %d", ret);
            return ret;
        }
    }
    if (callbacks == 0) {
        LOG_ERR("reset_all regression: no settings-reset callback registered");
        return -EINVAL;
    }

    int32_t got = 0;
    ret = zmk_custom_setting_get_int32(setting, &got);
    if (ret < 0) {
        LOG_ERR("reset_all regression: read-back failed: %d", ret);
        return ret;
    }
    if (got != default_value) {
        LOG_ERR("reset_all regression: value=%d, expected default %d after reset", got,
                default_value);
        return -EINVAL;
    }
    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("reset_all regression: unsaved state remained after reset");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_reset_all callbacks=%d value=%d", callbacks, got);
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_reset_all_test_init, APPLICATION, 99);

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
/*
 * A runtime-created keyspace entry (the storage a pool-backed namespace like
 * zmk-feature-runtime-macro's is built on) has no compile-time default, so
 * ZMK's official factory reset must make it CEASE TO EXIST, not just wipe its
 * flash record while leaving the slot in_use and its pool region occupied.
 * Create + persist an entry under the "test"/"macro/" keyspace, drive the SAME
 * reset iterator ZMK's core handler uses, and assert the entry is fully gone
 * (its slot released, so a lookup by key returns NULL) rather than lingering.
 */
static int custom_settings_reset_keyspace_test_init(void) {
    struct zmk_custom_setting_keyspace *keyspace =
        zmk_custom_settings_keyspace_find_for_key("test", "macro/reset");
    if (!keyspace) {
        LOG_ERR("reset_keyspace regression: no keyspace registered for test/macro/");
        return -ENOENT;
    }

    const struct zmk_custom_setting *created = NULL;
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(7);
    int ret = zmk_custom_setting_keyspace_create(keyspace, "macro/reset", &value,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &created);
    if (ret < 0 || !created) {
        LOG_ERR("reset_keyspace regression: create failed: %d", ret);
        return ret < 0 ? ret : -EINVAL;
    }
    if (!zmk_custom_setting_keyspace_find(keyspace, "macro/reset")) {
        LOG_ERR("reset_keyspace regression: entry not found after create");
        return -EINVAL;
    }

    /* Invoke the official reset exactly as ZMK's core subsystem does. */
    ZMK_RPC_SUBSYSTEM_SETTINGS_RESET_FOREACH(sub) {
        ret = sub->callback();
        if (ret < 0) {
            LOG_ERR("reset_keyspace regression: reset callback failed: %d", ret);
            return ret;
        }
    }

    if (zmk_custom_setting_keyspace_find(keyspace, "macro/reset")) {
        LOG_ERR("reset_keyspace regression: entry survived factory reset");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_reset_keyspace_deletes_entry key=macro/reset");
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_reset_keyspace_test_init, APPLICATION, 99);
#endif
