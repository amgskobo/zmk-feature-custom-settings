/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/custom_settings/custom_settings.pb.h>
#include <cormoran/zmk/custom_settings/custom_settings_relay.pb.h>
#include <zmk/workqueue.h>
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC) && IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/core.h>
#include <zmk/studio/custom.h>
#define ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC 1
#else
#define ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC 0
#endif
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SUBSYSTEM_IDENTIFIER_STRING "cormoran_custom_settings"
#define LIST_SETTINGS_NOTIFICATION_DELAY                                                           \
    K_MSEC(CONFIG_ZMK_CUSTOM_SETTINGS_LIST_NOTIFICATION_DELAY_MS)
#define LIST_SETTINGS_RELAY_DELAY K_MSEC(20)
#define ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_OVERHEAD 2
#define ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE                                                 \
    (CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN - ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_OVERHEAD)

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static struct zmk_rpc_custom_subsystem_meta custom_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-custom-settings/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran_custom_settings, &custom_settings_meta,
                         custom_settings_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran_custom_settings,
                                         cormoran_zmk_custom_settings_Response);
#endif

static bool studio_is_unlocked(void) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    return zmk_studio_core_get_lock_state() == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
#else
    return true;
#endif
}

static bool needs_unlock(enum zmk_custom_setting_permission permission) {
    return permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE && !studio_is_unlocked();
}

static bool source_targets_local(uint32_t source) {
    return source == ZMK_CUSTOM_SETTING_SOURCE_LOCAL || source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
}

struct zmk_custom_settings_setting_ref {
    bool has_custom_subsystem_id;
    char custom_subsystem_id[CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN];
    bool has_key;
    char key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    bool has_source;
    uint32_t source;
    bool has_array_index;
    uint32_t array_index;
};

struct zmk_custom_settings_setting_scope {
    bool has_custom_subsystem_id;
    char custom_subsystem_id[CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN];
    bool has_key;
    char key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    bool has_key_prefix;
    char key_prefix[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    bool has_source;
    uint32_t source;
};

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
BUILD_ASSERT(CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN > ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_OVERHEAD,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for custom settings relay "
             "payloads");
BUILD_ASSERT(ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE <= UINT8_MAX,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too large for custom settings relay "
             "payload_size");

struct zmk_custom_settings_relay_request {
    uint8_t source;
    uint8_t payload_size;
    uint8_t payload[ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE];
} __packed;

struct zmk_custom_settings_relay_notification {
    uint8_t source;
    uint8_t payload_size;
    uint8_t payload[ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE];
} __packed;

BUILD_ASSERT(sizeof(struct zmk_custom_settings_relay_request) <=
                 CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for custom settings relay "
             "requests");
BUILD_ASSERT(sizeof(struct zmk_custom_settings_relay_notification) <=
                 CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for custom settings relay "
             "notifications");

ZMK_EVENT_DECLARE(zmk_custom_settings_relay_request);
ZMK_EVENT_DECLARE(zmk_custom_settings_relay_notification);
ZMK_EVENT_IMPL(zmk_custom_settings_relay_request);
ZMK_EVENT_IMPL(zmk_custom_settings_relay_notification);

ZMK_RELAY_EVENT_HANDLE(zmk_custom_settings_relay_request, csr, source);
ZMK_RELAY_EVENT_HANDLE(zmk_custom_settings_relay_notification, csn, source);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_custom_settings_relay_request, csr, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_custom_settings_relay_notification, csn, source);
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static uint32_t ref_source(const cormoran_zmk_custom_settings_SettingRef *ref) {
    return ref->has_source ? ref->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}

static bool source_targets_peripherals(uint32_t source) {
    return source != ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}
#endif

static uint32_t setting_ref_source(const struct zmk_custom_settings_setting_ref *ref) {
    return ref->has_source ? ref->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static uint32_t scope_source(const cormoran_zmk_custom_settings_SettingScope *scope) {
    return scope->has_source ? scope->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}
#endif

static uint32_t setting_scope_source(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_source ? scope->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}

static const char *
setting_ref_custom_subsystem_id(const struct zmk_custom_settings_setting_ref *ref) {
    return ref->has_custom_subsystem_id ? ref->custom_subsystem_id : NULL;
}

static const char *setting_ref_key(const struct zmk_custom_settings_setting_ref *ref) {
    return ref->has_key ? ref->key : NULL;
}

static const struct zmk_custom_setting *
setting_for_ref(const struct zmk_custom_settings_setting_ref *ref) {
    if (!ref->has_key) {
        return NULL;
    }

    /* IS_ENABLED-guarded (not a plain ref->has_array_index check) so the
     * zmk_custom_setting_find_array_element call - only defined in
     * src/custom_settings_array.c when CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY is
     * enabled - is provably dead code when the feature is off, matching the
     * zmk_custom_setting_is_array() fold pattern. */
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) && ref->has_array_index) {
        return zmk_custom_setting_find_array_element(setting_ref_custom_subsystem_id(ref),
                                                     setting_ref_key(ref), ref->array_index);
    }
    return zmk_custom_setting_find(setting_ref_custom_subsystem_id(ref), setting_ref_key(ref));
}

static const struct zmk_custom_setting *
array_for_ref(const struct zmk_custom_settings_setting_ref *ref) {
    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) || !ref->has_key) {
        return NULL;
    }

    return ref->has_array_index
               ? zmk_custom_setting_find_array_element(setting_ref_custom_subsystem_id(ref),
                                                       setting_ref_key(ref), ref->array_index)
               : zmk_custom_setting_find_array(setting_ref_custom_subsystem_id(ref),
                                               setting_ref_key(ref));
}

static const char *
setting_scope_custom_subsystem_id(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_custom_subsystem_id ? scope->custom_subsystem_id : NULL;
}

static const char *setting_scope_key(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_key ? scope->key : NULL;
}

static const char *setting_scope_key_prefix(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_key_prefix ? scope->key_prefix : NULL;
}

static size_t bounded_strlen(const char *str, size_t max_len) {
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }

    return len;
}

static void copy_string(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) {
        return;
    }

    size_t len = MIN(bounded_strlen(src, dest_size - 1), dest_size - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

static void set_error(cormoran_zmk_custom_settings_Response *resp, const char *message) {
    cormoran_zmk_custom_settings_ErrorResponse err =
        cormoran_zmk_custom_settings_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_zmk_custom_settings_Response_error_tag;
    resp->response_type.error = err;
}

static void set_status(cormoran_zmk_custom_settings_Response *resp, uint32_t affected_count,
                       const char *message) {
    cormoran_zmk_custom_settings_StatusResponse status =
        cormoran_zmk_custom_settings_StatusResponse_init_zero;
    status.affected_count = affected_count;
    snprintf(status.message, sizeof(status.message), "%s", message);
    resp->which_response_type = cormoran_zmk_custom_settings_Response_status_tag;
    resp->response_type.status = status;
}

static cormoran_zmk_custom_settings_SettingConfidentiality
proto_confidentiality(enum zmk_custom_setting_confidentiality confidentiality) {
    return (cormoran_zmk_custom_settings_SettingConfidentiality)confidentiality;
}

static cormoran_zmk_custom_settings_SettingPermission
proto_permission(enum zmk_custom_setting_permission permission) {
    return (cormoran_zmk_custom_settings_SettingPermission)permission;
}

static cormoran_zmk_custom_settings_SettingNotificationKind
proto_notification_kind(enum zmk_custom_setting_changed_kind kind) {
    switch (kind) {
    case ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED:
        return cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_VALUE_UPDATED;
    case ZMK_CUSTOM_SETTING_CHANGED_SAVED:
        return cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_SAVED;
    case ZMK_CUSTOM_SETTING_CHANGED_DISCARDED:
        return cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_DISCARDED;
    case ZMK_CUSTOM_SETTING_CHANGED_RESET:
        return cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_RESET;
    default:
        return cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_VALUE_UPDATED;
    }
}

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index);
static const char *custom_subsystem_identifier_for_index(uint32_t index);
#endif

/*
 * SettingValue.bytes_value/string_value are nanopb callback fields (see the
 * .options file), not fixed inline arrays, so GetSetting/ListSettings can
 * stream a value of any size instead of omitting one that does not fit
 * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE. This requires a matching *decode*
 * callback wherever a SettingValue is decoded (WriteSettingRequest.value,
 * CreateSettingRequest.value, and their relayed equivalents) since the field
 * has no `.bytes`/`.size` struct members to read directly - decode copies into
 * a bounded scratch buffer instead (see decode_into_bounded_scratch below).
 *
 * `struct bounded_decode_scratch` also doubles as the source for a small
 * *encode* callback (encode_bounded_scratch_value) used at the handful of
 * sites that must re-encode a value they just decoded into a different
 * message: the split relay forwarding a WriteSetting to a peripheral, and a
 * central republishing a relay-decoded Notification to the local Studio
 * session. Every decode call site below owns its own scratch instance; decode
 * happens synchronously while processing exactly one request/notification at a
 * time, but distinct instances per call site (rather than one shared global)
 * avoid relying on that being true forever.
 */
struct bounded_decode_scratch {
    uint8_t *buf;    /* capacity+1 bytes: the last byte is reserved for a NUL
                      * terminator (STRING convenience; harmless for BYTES). */
    size_t capacity; /* max payload bytes, not counting the reserved NUL. */
    size_t size;     /* actual decoded payload length. */
};

static bool decode_into_bounded_scratch(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct bounded_decode_scratch *scratch = *arg;
    size_t size = stream->bytes_left;
    if (size > scratch->capacity) {
        return false;
    }
    if (!pb_read(stream, scratch->buf, size)) {
        return false;
    }
    scratch->buf[size] = '\0';
    scratch->size = size;

    /* nanopb never sets a oneof's which_<oneof> for PB_ATYPE_CALLBACK arms
     * (decode_field only does that for STATIC arms; decode_callback_field
     * does not touch it) - the callback is expected to record which arm
     * arrived itself. Without this, which_value_type stays 0 after a
     * bytes/string payload decode and proto_to_value() rejects the request
     * (-EINVAL, seen on real hardware as "Invalid request"). field->message
     * points at the enclosing SettingValue and field->tag is the arm's tag
     * (bytes_value/string_value). */
    cormoran_zmk_custom_settings_SettingValue *value = field->message;
    value->which_value_type = field->tag;
    return true;
}

/* Wire `value`'s bytes_value/string_value callback (they alias the same
 * union member, see the generated cormoran_zmk_custom_settings_SettingValue)
 * to decode into `scratch`.
 *
 * CAUTION: only call this on a SettingValue that pb_decode() reaches WITHOUT
 * crossing a oneof submessage arm (e.g. a bare Setting), or from inside a
 * message-level cb_<oneof> precallback (see *_arm_wire_precallback below).
 * nanopb memsets a oneof's submessage arm to zero when the arm's tag is
 * first seen during decode, wiping any callback wired in advance - the value
 * field is then silently skipped and which_value_type stays 0. Found on real
 * hardware (CreateSetting with a STRING value -> "Invalid request"); the
 * submsg_callback proto option + these precallbacks are the nanopb-sanctioned
 * fix. */
static void wire_setting_value_decode(cormoran_zmk_custom_settings_SettingValue *value,
                                      struct bounded_decode_scratch *scratch) {
    scratch->size = 0;
    value->value_type.bytes_value.funcs.decode = decode_into_bounded_scratch;
    value->value_type.bytes_value.arg = scratch;
}

/* Message-level oneof precallback (nanopb submsg_callback option on
 * Request): invoked when a request_type arm's tag is known, after nanopb
 * zeroed the arm but before the arm submessage is decoded - the only point
 * where wiring a callback inside the arm sticks. *arg is the
 * bounded_decode_scratch the inner SettingValue payload should land in. */
static bool request_arm_wire_precallback(pb_istream_t *stream, const pb_field_t *field,
                                         void **arg) {
    (void)stream;
    struct bounded_decode_scratch *scratch = *arg;
    if (field->tag == cormoran_zmk_custom_settings_Request_write_setting_tag) {
        cormoran_zmk_custom_settings_WriteSettingRequest *req = field->pData;
        wire_setting_value_decode(&req->value, scratch);
    } else if (field->tag == cormoran_zmk_custom_settings_Request_create_setting_tag) {
        cormoran_zmk_custom_settings_CreateSettingRequest *req = field->pData;
        wire_setting_value_decode(&req->value, scratch);
    }
    return true;
}

/* Only the split central re-encodes relayed values; the handler self-test
 * (tests/support/custom_settings_handler_assertions_2.inc) drives the same
 * re-encode helpers without a relay. */
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC &&                                                        \
    ((IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                    \
      IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) ||                                                \
     IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST))
/* A relayed Setting.default_value BYTES/STRING payload needs its own decode
 * scratch, distinct from the value scratch, since a single Setting can carry
 * both a BYTES/STRING value and a BYTES/STRING default at once. A default always
 * fits the fixed carrier, so this is sized to VALUE_MAX_SIZE. */
static uint8_t relay_notification_default_decode_buf[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
static struct bounded_decode_scratch relay_notification_default_decode_scratch = {
    .buf = relay_notification_default_decode_buf,
    .capacity = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
};

/* Same for Notification.notification_type's setting arm (a relayed
 * peripheral notification decoded on the central, see
 * relayed_notification_to_public). */
static bool notification_arm_wire_precallback(pb_istream_t *stream, const pb_field_t *field,
                                              void **arg) {
    (void)stream;
    struct bounded_decode_scratch *scratch = *arg;
    if (field->tag == cormoran_zmk_custom_settings_Notification_setting_tag) {
        cormoran_zmk_custom_settings_SettingNotification *notification = field->pData;
        wire_setting_value_decode(&notification->setting.value, scratch);
        /* The optional default_value carries its own BYTES/STRING payload and
         * needs a separate scratch (both can be present at once). The value
         * scratch arrives via *arg; the default scratch is a file static
         * referenced directly. */
        wire_setting_value_decode(&notification->setting.default_value,
                                  &relay_notification_default_decode_scratch);
    }
    return true;
}

static bool encode_bounded_scratch_value(pb_ostream_t *stream, const pb_field_t *field,
                                         void *const *arg) {
    const struct bounded_decode_scratch *scratch = *arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_string(stream, scratch->buf, scratch->size);
}

/* Re-target an already-decoded value's bytes_value/string_value callback from
 * decode role to encode role, streaming the same bytes back out. A no-op for
 * every which_value_type other than bytes_value/string_value. */
static void retarget_value_to_encode_scratch(cormoran_zmk_custom_settings_SettingValue *value,
                                             struct bounded_decode_scratch *scratch) {
    if (value->which_value_type != cormoran_zmk_custom_settings_SettingValue_bytes_value_tag &&
        value->which_value_type != cormoran_zmk_custom_settings_SettingValue_string_value_tag) {
        return;
    }
    value->value_type.bytes_value.funcs.encode = encode_bounded_scratch_value;
    value->value_type.bytes_value.arg = scratch;
}
#endif

/* Encodes a live setting's BYTES/STRING scalar payload at pb_encode time (small
 * carrier or a large-store pool region), so GetSetting is not capped at
 * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE. Small and large values share this
 * one path. The RPC serializer converter (zmk_custom_setting_serialize_rpc_value)
 * is applied only when the value still fits the fixed carrier - it is bounded
 * by that carrier and is not applied to the >carrier path.
 *
 * The value is copied out before anything is written to the stream. The stream
 * is paced by the host - one 27-byte indication per confirmation over
 * Bluetooth - so the write can wait for seconds, or until the link goes. No
 * settings lock may be held across that wait: Zephyr's settings lock and this
 * module's state mutex are taken by saves, loads and every RAM read, some of
 * them on the system work queue, which the watchdog restarts the keyboard for
 * if it stalls. */
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES)
/* Every encode runs inside send_response(), which the transport mutex already
 * serializes, so this lock is never contended there. It only keeps the one
 * staging buffer safe should that ever change, and nothing is acquired under
 * it that a settings writer could be waiting on. */
static K_MUTEX_DEFINE(large_encode_lock);
static uint8_t large_encode_buf[CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE];
#endif

static bool encode_setting_scalar_value(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const struct zmk_custom_setting *setting = *arg;
    if (!setting) {
        return true;
    }

    struct zmk_custom_setting_value value;
    struct zmk_custom_setting_value rpc_value;
    int ret = zmk_custom_setting_read(setting, &value);
    if (ret == 0) {
        ret = zmk_custom_setting_serialize_rpc_value(setting, &value, &rpc_value);
        if (ret < 0) {
            return false;
        }
        const uint8_t *data = rpc_value.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES
                                  ? rpc_value.bytes_value
                                  : (const uint8_t *)rpc_value.string_value;
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        return pb_encode_string(stream, data, rpc_value.size);
    }
    if (ret != -EMSGSIZE) {
        return false;
    }

    /* -EMSGSIZE above only ever happens for a >carrier (pool-backed) value,
     * which cannot exist when CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES is off -
     * but that is a runtime property the compiler cannot see, so the staging
     * buffer and this path exist only when the option is on. */
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES)
    /* Large value (> carrier): read_into() copies the payload under the state
     * mutex alone and releases it before returning; only then is the stream
     * written. No per-setting max_size exceeds the staging buffer, so a value
     * that no longer fits (-EMSGSIZE) can only mean the setting changed kind
     * underneath us, and is reported as a failed encode. */
    bool ok = false;

    k_mutex_lock(&large_encode_lock, K_FOREVER);
    size_t size = 0;
    ret = zmk_custom_setting_read_into(setting, large_encode_buf, sizeof(large_encode_buf), &size,
                                       NULL);
    if (ret == 0) {
        ok = pb_encode_tag_for_field(stream, field) &&
             pb_encode_string(stream, large_encode_buf, size);
    }
    k_mutex_unlock(&large_encode_lock);

    return ok;
#else
    return false;
#endif
}

/* Streams a BYTES/STRING setting's DEFAULT value (not its current value) into a
 * SettingValue bytes_value/string_value callback field. A default always fits
 * the fixed carrier (it is a registration-time value, never pool-backed), so no
 * large-store streaming path is needed here - unlike encode_setting_scalar_value
 * for the current value. */
static bool encode_setting_default_value(pb_ostream_t *stream, const pb_field_t *field,
                                         void *const *arg) {
    const struct zmk_custom_setting *setting = *arg;
    if (!setting) {
        return true;
    }

    struct zmk_custom_setting_value value;
    struct zmk_custom_setting_value rpc_value;
    int ret = zmk_custom_setting_read_default(setting, &value);
    if (ret < 0) {
        return false;
    }
    ret = zmk_custom_setting_serialize_rpc_value(setting, &value, &rpc_value);
    if (ret < 0) {
        return false;
    }
    const uint8_t *data = rpc_value.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES
                              ? rpc_value.bytes_value
                              : (const uint8_t *)rpc_value.string_value;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_string(stream, data, rpc_value.size);
}

static int scalar_proto_to_value(const cormoran_zmk_custom_settings_SettingScalarValue *src,
                                 struct zmk_custom_setting_value *dest) {
    *dest = (struct zmk_custom_setting_value){0};

    switch (src->which_value_type) {
    case cormoran_zmk_custom_settings_SettingScalarValue_bytes_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES;
        dest->size = src->value_type.bytes_value.size;
        if (dest->size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        memcpy(dest->bytes_value, src->value_type.bytes_value.bytes, dest->size);
        return 0;
    case cormoran_zmk_custom_settings_SettingScalarValue_int32_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
        dest->int32_value = src->value_type.int32_value;
        return 0;
    case cormoran_zmk_custom_settings_SettingScalarValue_bool_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL;
        dest->bool_value = src->value_type.bool_value;
        return 0;
    case cormoran_zmk_custom_settings_SettingScalarValue_string_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
        dest->size =
            bounded_strlen(src->value_type.string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        copy_string(dest->string_value, sizeof(dest->string_value), src->value_type.string_value);
        return 0;
    case cormoran_zmk_custom_settings_SettingScalarValue_behavior_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR;
        dest->behavior_value.behavior_id = src->value_type.behavior_value.behavior_id;
        dest->behavior_value.param1 = src->value_type.behavior_value.param1;
        dest->behavior_value.param2 = src->value_type.behavior_value.param2;
        return 0;
    default:
        return -EINVAL;
    }
}

/* `value_scratch` must be the same struct bounded_decode_scratch that was
 * wired via wire_setting_value_decode() before src's enclosing message was
 * pb_decode()-d (NULL if src's bytes_value/string_value was never wired,
 * e.g. a value manufactured in-process rather than decoded off the wire -
 * bytes_value/string_value then fail with -EINVAL). */
static int proto_to_value(const cormoran_zmk_custom_settings_SettingValue *src,
                          const struct bounded_decode_scratch *value_scratch,
                          struct zmk_custom_setting_value *dest, bool *is_array,
                          uint32_t *array_index, uint32_t *array_size) {
    *dest = (struct zmk_custom_setting_value){0};
    *is_array = false;

    switch (src->which_value_type) {
    case cormoran_zmk_custom_settings_SettingValue_bytes_value_tag:
    case cormoran_zmk_custom_settings_SettingValue_string_value_tag:
        if (!value_scratch) {
            return -EINVAL;
        }
        /* decode_into_bounded_scratch already rejected anything past the
         * scratch's own capacity; CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE is
         * that capacity for every call site that feeds this function - a value
         * bigger than one frame still needs WriteValueChunk. */
        dest->size = value_scratch->size;
        if (src->which_value_type == cormoran_zmk_custom_settings_SettingValue_bytes_value_tag) {
            dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES;
            memcpy(dest->bytes_value, value_scratch->buf, dest->size);
        } else {
            dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
            copy_string(dest->string_value, sizeof(dest->string_value),
                        (const char *)value_scratch->buf);
        }
        return 0;
    case cormoran_zmk_custom_settings_SettingValue_int32_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
        dest->int32_value = src->value_type.int32_value;
        return 0;
    case cormoran_zmk_custom_settings_SettingValue_bool_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL;
        dest->bool_value = src->value_type.bool_value;
        return 0;
    case cormoran_zmk_custom_settings_SettingValue_behavior_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR;
        dest->behavior_value.behavior_id = src->value_type.behavior_value.behavior_id;
        dest->behavior_value.param1 = src->value_type.behavior_value.param1;
        dest->behavior_value.param2 = src->value_type.behavior_value.param2;
        return 0;
    case cormoran_zmk_custom_settings_SettingValue_array_value_tag:
        if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) ||
            !src->value_type.array_value.has_value) {
            return -EINVAL;
        }
        *is_array = true;
        *array_index = src->value_type.array_value.index;
        *array_size = src->value_type.array_value.size;
        return scalar_proto_to_value(&src->value_type.array_value.value, dest);
    default:
        return -EINVAL;
    }
}

static int value_to_scalar_proto(const struct zmk_custom_setting_value *src,
                                 cormoran_zmk_custom_settings_SettingScalarValue *dest) {
    *dest = (cormoran_zmk_custom_settings_SettingScalarValue)
        cormoran_zmk_custom_settings_SettingScalarValue_init_zero;

    switch (src->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingScalarValue_bytes_value_tag;
        dest->value_type.bytes_value.size = src->size;
        if (src->size > sizeof(dest->value_type.bytes_value.bytes)) {
            return -EMSGSIZE;
        }
        memcpy(dest->value_type.bytes_value.bytes, src->bytes_value, src->size);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingScalarValue_int32_value_tag;
        dest->value_type.int32_value = src->int32_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingScalarValue_bool_value_tag;
        dest->value_type.bool_value = src->bool_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingScalarValue_string_value_tag;
        copy_string(dest->value_type.string_value, sizeof(dest->value_type.string_value),
                    src->string_value);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingScalarValue_behavior_value_tag;
        dest->value_type.behavior_value.behavior_id = src->behavior_value.behavior_id;
        dest->value_type.behavior_value.param1 = src->behavior_value.param1;
        dest->value_type.behavior_value.param2 = src->behavior_value.param2;
        return 0;
    default:
        return -EINVAL;
    }
}

static int value_to_proto(const struct zmk_custom_setting *setting,
                          const struct zmk_custom_setting_value *src,
                          cormoran_zmk_custom_settings_SettingValue *dest) {
    *dest = (cormoran_zmk_custom_settings_SettingValue)
        cormoran_zmk_custom_settings_SettingValue_init_zero;

    struct zmk_custom_setting_value rpc_value;
    int ret = zmk_custom_setting_serialize_rpc_value(setting, src, &rpc_value);
    if (ret < 0) {
        return ret;
    }
    src = &rpc_value;

    if (zmk_custom_setting_is_array(setting)) {
        dest->which_value_type = cormoran_zmk_custom_settings_SettingValue_array_value_tag;
        dest->value_type.array_value.index = setting->array_index;
        dest->value_type.array_value.size = zmk_custom_setting_array_size(setting);
        dest->value_type.array_value.has_value = true;
        return value_to_scalar_proto(src, &dest->value_type.array_value.value);
    }

    cormoran_zmk_custom_settings_SettingScalarValue scalar =
        cormoran_zmk_custom_settings_SettingScalarValue_init_zero;
    ret = value_to_scalar_proto(src, &scalar);
    if (ret < 0) {
        return ret;
    }

    /* BYTES/STRING never reach here: setting_value_to_proto intercepts those
     * (non-array) before calling value_to_proto and streams them through
     * encode_setting_scalar_value instead - this function's non-array branch
     * only ever sees INT32/BOOL/BEHAVIOR, whose RPC-serialized form (the RPC
     * serializer never changes a value's type) is small enough to copy
     * inline. */
    switch (scalar.which_value_type) {
    case cormoran_zmk_custom_settings_SettingScalarValue_int32_value_tag:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingValue_int32_value_tag;
        dest->value_type.int32_value = scalar.value_type.int32_value;
        return 0;
    case cormoran_zmk_custom_settings_SettingScalarValue_bool_value_tag:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingValue_bool_value_tag;
        dest->value_type.bool_value = scalar.value_type.bool_value;
        return 0;
    case cormoran_zmk_custom_settings_SettingScalarValue_behavior_value_tag:
        dest->which_value_type = cormoran_zmk_custom_settings_SettingValue_behavior_value_tag;
        dest->value_type.behavior_value = scalar.value_type.behavior_value;
        return 0;
    default:
        return -EINVAL;
    }
}

static int constraint_to_proto(const struct zmk_custom_setting_constraint *src,
                               cormoran_zmk_custom_settings_SettingConstraint *dest) {
    *dest = (cormoran_zmk_custom_settings_SettingConstraint)
        cormoran_zmk_custom_settings_SettingConstraint_init_zero;

    switch (src->type) {
    case ZMK_CUSTOM_SETTING_CONSTRAINT_NONE:
        return -ENOENT;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE: {
        dest->which_constraint_type = cormoran_zmk_custom_settings_SettingConstraint_range_tag;
        dest->constraint_type.range.has_min = true;
        dest->constraint_type.range.has_max = true;
        int ret = value_to_scalar_proto(&src->range.min, &dest->constraint_type.range.min);
        if (ret < 0) {
            return ret;
        }
        return value_to_scalar_proto(&src->range.max, &dest->constraint_type.range.max);
    }
    case ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS:
        dest->which_constraint_type = cormoran_zmk_custom_settings_SettingConstraint_options_tag;
        dest->constraint_type.options.values_count =
            MIN(src->options.count, ARRAY_SIZE(dest->constraint_type.options.values));
        dest->constraint_type.options.labels_count =
            MIN(src->options.count, ARRAY_SIZE(dest->constraint_type.options.labels));
        for (size_t i = 0; i < dest->constraint_type.options.values_count; i++) {
            int ret = value_to_scalar_proto(&src->options.values[i],
                                            &dest->constraint_type.options.values[i]);
            if (ret < 0) {
                return ret;
            }
            if (src->options.labels) {
                copy_string(dest->constraint_type.options.labels[i],
                            sizeof(dest->constraint_type.options.labels[i]),
                            src->options.labels[i]);
            }
        }
        return 0;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE:
        dest->which_constraint_type = cormoran_zmk_custom_settings_SettingConstraint_hid_usage_tag;
        dest->constraint_type.hid_usage.usage_page = src->hid_usage.usage_page;
        dest->constraint_type.hid_usage.usage_min = src->hid_usage.usage_min;
        dest->constraint_type.hid_usage.usage_max = src->hid_usage.usage_max;
        return 0;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID:
        dest->which_constraint_type = cormoran_zmk_custom_settings_SettingConstraint_layer_id_tag;
        return 0;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID:
        dest->which_constraint_type =
            cormoran_zmk_custom_settings_SettingConstraint_behavior_id_tag;
        return 0;
    default:
        return -EINVAL;
    }
}

static int setting_meta_to_proto(const struct zmk_custom_setting *setting,
                                 cormoran_zmk_custom_settings_SettingMeta *dest) {
    *dest = (cormoran_zmk_custom_settings_SettingMeta)
        cormoran_zmk_custom_settings_SettingMeta_init_zero;

    dest->confidentiality = proto_confidentiality(setting->confidentiality);
    dest->read_permission = proto_permission(setting->read_permission);
    dest->write_permission = proto_permission(setting->write_permission);

    /* A keyspace slot's own constraints are always empty (they describe its
     * opaque blob, which has none of its own) - present the owning
     * keyspace's PAYLOAD constraints instead, matching what
     * zmk_custom_setting_write validates a write against. */
    const struct zmk_custom_setting_keyspace *keyspace = zmk_custom_setting_keyspace_of(setting);
    const struct zmk_custom_setting_constraint *constraints =
        keyspace ? keyspace->constraints : setting->constraints;
    size_t constraints_count = keyspace ? keyspace->constraints_count : setting->constraints_count;

    for (size_t i = 0;
         i < constraints_count && dest->constraints_count < ARRAY_SIZE(dest->constraints); i++) {
        if (constraints[i].type == ZMK_CUSTOM_SETTING_CONSTRAINT_NONE) {
            continue;
        }

        int ret = constraint_to_proto(&constraints[i], &dest->constraints[dest->constraints_count]);
        if (ret < 0) {
            continue;
        }
        dest->constraints_count++;
    }

    return 0;
}

/* A keyspace slot's OWN value_type is always BYTES internally (the opaque
 * [user_key\0][payload] blob - see zmk_custom_setting_keyspace_of); the
 * PRESENTED type - what RPC clients see the value as - is the owning
 * keyspace's declared value_type. Every other setting presents as its own
 * value_type. */
static enum zmk_custom_setting_value_type
presented_value_type(const struct zmk_custom_setting *setting) {
    const struct zmk_custom_setting_keyspace *keyspace = zmk_custom_setting_keyspace_of(setting);
    return keyspace ? keyspace->value_type : setting->value_type;
}

static int setting_value_to_proto(const struct zmk_custom_setting *setting,
                                  cormoran_zmk_custom_settings_SettingValue *dest) {
    *dest = (cormoran_zmk_custom_settings_SettingValue)
        cormoran_zmk_custom_settings_SettingValue_init_zero;

    enum zmk_custom_setting_value_type presented_type = presented_value_type(setting);
    if (!zmk_custom_setting_is_array(setting) &&
        (presented_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
         presented_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING)) {
        /* Defer the actual read to pb_encode time rather than reading eagerly
         * here - see encode_setting_scalar_value, which is keyspace-aware
         * (zmk_custom_setting_read/with_large_raw_bytes already strip a
         * keyspace slot's key prefix), so this callback wiring needs no further
         * change for keyspace slots. The oneof arm (bytes_value vs
         * string_value) depends only on the PRESENTED value_type, which is
         * fixed at registration, so it is safe to select now even though the
         * bytes themselves are read later. */
        dest->which_value_type = presented_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES
                                     ? cormoran_zmk_custom_settings_SettingValue_bytes_value_tag
                                     : cormoran_zmk_custom_settings_SettingValue_string_value_tag;
        dest->value_type.bytes_value.funcs.encode = encode_setting_scalar_value;
        dest->value_type.bytes_value.arg = (void *)setting;
        return 0;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        return ret;
    }

    return value_to_proto(setting, &value, dest);
}

/* Fills Setting.default_value with the setting's default. Mirrors
 * setting_value_to_proto: BYTES/STRING defer to a pb_encode-time callback (the
 * oneof arm depends only on the fixed presented type), scalars are read and
 * inlined now. Callers must have already excluded arrays and keyspace slots
 * (they have no compile-time default). */
static int setting_default_to_proto(const struct zmk_custom_setting *setting,
                                    cormoran_zmk_custom_settings_SettingValue *dest) {
    *dest = (cormoran_zmk_custom_settings_SettingValue)
        cormoran_zmk_custom_settings_SettingValue_init_zero;

    enum zmk_custom_setting_value_type presented_type = presented_value_type(setting);
    if (presented_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
        presented_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        dest->which_value_type = presented_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES
                                     ? cormoran_zmk_custom_settings_SettingValue_bytes_value_tag
                                     : cormoran_zmk_custom_settings_SettingValue_string_value_tag;
        dest->value_type.bytes_value.funcs.encode = encode_setting_default_value;
        dest->value_type.bytes_value.arg = (void *)setting;
        return 0;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_default(setting, &value);
    if (ret < 0) {
        return ret;
    }

    return value_to_proto(setting, &value, dest);
}

static int setting_to_proto(const struct zmk_custom_setting *setting,
                            cormoran_zmk_custom_settings_Setting *dest, bool include_value,
                            bool include_meta, bool include_default, uint32_t source) {
    *dest = (cormoran_zmk_custom_settings_Setting)cormoran_zmk_custom_settings_Setting_init_zero;

    int ret;
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    uint32_t custom_subsystem_index = 0;
    ret = custom_subsystem_index_for_identifier(setting->custom_subsystem_id,
                                                &custom_subsystem_index);
    if (ret < 0) {
        return ret;
    }
    dest->custom_subsystem_index = custom_subsystem_index;
#else
    dest->custom_subsystem_index = 0;
#endif

    copy_string(dest->key, sizeof(dest->key), zmk_custom_setting_public_key(setting));
    dest->has_unsaved_value = zmk_custom_setting_has_unsaved_value(setting);
    dest->source = source;
    if (include_meta) {
        dest->has_meta = true;
        ret = setting_meta_to_proto(setting, &dest->meta);
        if (ret < 0) {
            dest->has_meta = false;
            return ret;
        }
    }

    if (include_value &&
        setting->confidentiality != ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE) {
        dest->has_value = true;
        /* A BYTES/STRING value of any size streams through
         * encode_setting_scalar_value at pb_encode time (see
         * setting_value_to_proto), so there is no "too large for one frame,
         * omit it" case here for a direct (non-relay) response/notification -
         * that omission is only needed on the bandwidth-bounded split relay
         * path (see the retry-without-value fallback in
         * raise_setting_notification). */
        ret = setting_value_to_proto(setting, &dest->value);
        if (ret < 0) {
            dest->has_value = false;
            return ret;
        }
    }

    /* Expose the default only when the client asked for it, the value itself is
     * exposable (same confidentiality/lock gate), and the current value differs
     * from the default. Arrays and keyspace slots have no compile-time default.
     * The read_default probe simply confirms a default exists (returns -ENOENT
     * otherwise); the actual bytes are read/streamed in setting_default_to_proto
     * / encode_setting_default_value. */
    struct zmk_custom_setting_value default_probe;
    if (include_default && include_value &&
        setting->confidentiality != ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE &&
        !zmk_custom_setting_is_array(setting) && !zmk_custom_setting_keyspace_of(setting) &&
        zmk_custom_setting_read_default(setting, &default_probe) == 0 &&
        !zmk_custom_setting_matches_default(setting)) {
        dest->has_default_value = true;
        ret = setting_default_to_proto(setting, &dest->default_value);
        if (ret < 0) {
            dest->has_default_value = false;
            return ret;
        }
    }

    return 0;
}

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static const char *custom_subsystem_identifier_for_index(uint32_t index) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    if (index >= subsystem_count) {
        return NULL;
    }

    struct zmk_rpc_custom_subsystem *custom_subsys;
    STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, index, &custom_subsys);
    return custom_subsys->identifier;
}

static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index) {
    if (!identifier) {
        return -ENOENT;
    }

    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, identifier) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int custom_subsystem_index(void) {
    uint32_t index;
    int ret = custom_subsystem_index_for_identifier(SUBSYSTEM_IDENTIFIER_STRING, &index);
    if (ret < 0) {
        return ret;
    }

    return (int)index;
}

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const cormoran_zmk_custom_settings_Notification *notification =
        (const cormoran_zmk_custom_settings_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_zmk_custom_settings_Notification_fields, notification);
}
#endif

static K_MUTEX_DEFINE(notification_buffer_lock);
static cormoran_zmk_custom_settings_Notification notification_buffer;
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static struct zmk_custom_settings_relay_notification notification_relay_event_buffer;
static cormoran_zmk_custom_settings_RelayNotification notification_relay_buffer;
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static K_MUTEX_DEFINE(relay_notification_decode_lock);
static cormoran_zmk_custom_settings_RelayNotification relay_notification_decode_buffer;
/* Sized to the relay envelope, not CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE:
 * a peripheral may include a value up to whatever fits
 * ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE (see the retry-without-value
 * fallback in raise_setting_notification), which can exceed one RPC frame's
 * worth depending on CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN. */
static uint8_t relay_notification_value_decode_buf[ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE + 1];
static struct bounded_decode_scratch relay_notification_value_decode_scratch = {
    .buf = relay_notification_value_decode_buf,
    .capacity = ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE,
};

static int relayed_notification_to_public(const struct zmk_custom_settings_relay_notification *ev,
                                          cormoran_zmk_custom_settings_Notification *notification) {
    k_mutex_lock(&relay_notification_decode_lock, K_FOREVER);

    cormoran_zmk_custom_settings_RelayNotification *relay = &relay_notification_decode_buffer;
    *relay = (cormoran_zmk_custom_settings_RelayNotification)
        cormoran_zmk_custom_settings_RelayNotification_init_zero;
    /* Wired via Notification's message-level oneof precallback, not in
     * advance - see request_arm_wire_precallback's comment (nanopb zeroes
     * the selected notification_type arm during decode). */
    relay_notification_value_decode_scratch.size = 0;
    relay->notification.cb_notification_type.funcs.decode = notification_arm_wire_precallback;
    relay->notification.cb_notification_type.arg = &relay_notification_value_decode_scratch;
    pb_istream_t stream = pb_istream_from_buffer(ev->payload, ev->payload_size);
    if (!pb_decode(&stream, cormoran_zmk_custom_settings_RelayNotification_fields, relay)) {
        LOG_WRN("Failed to decode relayed custom settings notification: %s", PB_GET_ERROR(&stream));
        k_mutex_unlock(&relay_notification_decode_lock);
        return -EIO;
    }

    *notification = relay->notification;
    /* CRITICAL: the struct copy above brought along cb_notification_type,
     * whose funcs union still holds the DECODE-role precallback wired before
     * pb_decode(). nanopb's encoder invokes cb_<oneof>.funcs.encode for
     * PB_LTYPE_SUBMSG_W_CB arms too (pb_encode.c, "Message callback is
     * stored right before pSize"), so re-encoding this notification with the
     * stale callback in place makes the precallback run in encode context
     * and overwrite the SettingValue union with the decode callback's
     * pointers - observed on real hardware as every relayed INT32 turning
     * into 362819 (0x58943, the Thumb address of decode_into_bounded_scratch)
     * and every relayed BYTES/STRING value vanishing. Clear it so the
     * republish encode sees no message-level callback. */
    notification->cb_notification_type = (pb_callback_t){0};
    if (notification->which_notification_type !=
            cormoran_zmk_custom_settings_Notification_setting_tag ||
        !notification->notification_type.setting.has_setting) {
        k_mutex_unlock(&relay_notification_decode_lock);
        return 0;
    }

    /* notification->notification_type.setting.setting.value's
     * bytes_value/string_value (if that is the active oneof arm) is still
     * wired for DECODE at this point (it was just copied out of `relay`
     * above) - retarget it to stream the captured bytes back out, since the
     * caller (relay_notification_work_handler) republishes `notification` to
     * the local Studio session via a fresh pb_encode. */
    if (notification->notification_type.setting.setting.has_value) {
        retarget_value_to_encode_scratch(&notification->notification_type.setting.setting.value,
                                         &relay_notification_value_decode_scratch);
    }
    /* Same retarget for the optional default_value, whose BYTES/STRING arm was
     * decoded into its own scratch above. */
    if (notification->notification_type.setting.setting.has_default_value) {
        retarget_value_to_encode_scratch(
            &notification->notification_type.setting.setting.default_value,
            &relay_notification_default_decode_scratch);
    }

    if (relay->has_custom_subsystem_id) {
        uint32_t custom_subsystem_index = 0;
        int ret = custom_subsystem_index_for_identifier(relay->custom_subsystem_id,
                                                        &custom_subsystem_index);
        if (ret < 0) {
            k_mutex_unlock(&relay_notification_decode_lock);
            return ret;
        }
        notification->notification_type.setting.setting.custom_subsystem_index =
            custom_subsystem_index;
    }
    notification->notification_type.setting.setting.source = ev->source;

    k_mutex_unlock(&relay_notification_decode_lock);
    return 0;
}

static int
raise_encoded_studio_notification(const cormoran_zmk_custom_settings_Notification *notification) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
        .arg = (void *)notification,
    };

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
}
#endif

/*
 * Studio-notification suppression for RPC-originated mutations.
 *
 * A mutating Studio RPC (save/discard/reset/write/create/delete) already
 * confirms success to the single-connection Studio client through its own RPC
 * response. The change events these mutations raise synchronously would, left
 * unguarded, queue a redundant self-notification onto the low-prio workqueue;
 * on the shared single-connection USB CDC Studio transport that notification
 * races and starves the still-in-flight RPC response, so the client times out
 * even though the mutation and its flash write already succeeded.
 *
 * We therefore suppress locally-raised notifications for the duration of RPC
 * request dispatch. A depth COUNTER (not a bool) is required because a single
 * request nests change events: e.g. save_settings raises one SAVED event per
 * affected setting, synchronously, inside the same dispatch. Dispatch is
 * single-threaded - Studio processes one request at a time on its RPC thread
 * and the change-event listeners fire synchronously on that same thread - so a
 * plain int needs no atomics.
 *
 * Only the RPC entry point (custom_settings_rpc_handle_request) brackets
 * dispatch, so non-RPC sources keep notifying: boot settings-load, direct
 * firmware-API mutations, and split-relay writes on a peripheral never pass
 * through that bracket and their notifications must still reach Studio.
 */
static int notify_suppress_depth;

static inline void notify_suppress_begin(void) { notify_suppress_depth++; }

static inline void notify_suppress_end(void) {
    if (notify_suppress_depth > 0) {
        notify_suppress_depth--;
    }
}

static inline bool notify_suppressed(void) { return notify_suppress_depth > 0; }

/* Public wrappers (declared in custom_settings.h) so a module that mutates
 * custom settings from inside its own Studio RPC subsystem handler can suppress
 * the redundant self-notification, the same way this module brackets its own
 * RPC dispatch below. Defined only when the header exposes them as extern (the
 * CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=n build gets inline no-ops instead), so
 * a peripheral-relay build that compiles this file without local Studio RPC
 * does not collide with those inline definitions. */
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC)
void zmk_custom_settings_notify_suppress_begin(void) { notify_suppress_begin(); }
void zmk_custom_settings_notify_suppress_end(void) { notify_suppress_end(); }
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST)
/* Counts notifications that were actually queued (i.e. not suppressed).
 * Consumed only by the RPC-suppression self-test below; incrementing is
 * test-gated so production builds are unaffected. */
static uint32_t notify_submit_count_for_test;
#endif

static int raise_setting_notification(const struct zmk_custom_setting *setting,
                                      cormoran_zmk_custom_settings_SettingNotificationKind kind,
                                      bool include_value, bool include_meta, bool include_default,
                                      uint32_t source) {
    k_mutex_lock(&notification_buffer_lock, K_FOREVER);

    cormoran_zmk_custom_settings_Notification *notification = &notification_buffer;
    *notification = (cormoran_zmk_custom_settings_Notification)
        cormoran_zmk_custom_settings_Notification_init_zero;
    notification->which_notification_type = cormoran_zmk_custom_settings_Notification_setting_tag;
    notification->notification_type.setting.kind = kind;
    notification->notification_type.setting.has_setting = true;
    int ret = setting_to_proto(setting, &notification->notification_type.setting.setting,
                               include_value, include_meta, include_default, source);
    if (ret < 0) {
        k_mutex_unlock(&notification_buffer_lock);
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_custom_settings_relay_notification *relay_notification =
        &notification_relay_event_buffer;
    *relay_notification = (struct zmk_custom_settings_relay_notification){
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    };
    cormoran_zmk_custom_settings_RelayNotification *relay = &notification_relay_buffer;
    *relay = (cormoran_zmk_custom_settings_RelayNotification)
        cormoran_zmk_custom_settings_RelayNotification_init_zero;
    relay->has_custom_subsystem_id = true;
    copy_string(relay->custom_subsystem_id, sizeof(relay->custom_subsystem_id),
                setting->custom_subsystem_id);
    relay->has_notification = true;
    relay->notification = *notification;
    pb_ostream_t stream =
        pb_ostream_from_buffer(relay_notification->payload, sizeof(relay_notification->payload));
    bool encoded = pb_encode(&stream, cormoran_zmk_custom_settings_RelayNotification_fields, relay);
    if (!encoded && (relay->notification.notification_type.setting.setting.has_value ||
                     relay->notification.notification_type.setting.setting.has_default_value)) {
        /* Large values are not relayed: unlike the direct GetSetting/list
         * path, which streams a value of any size, the relay envelope is a
         * fixed CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN buffer. Retry once with
         * the value (and the optional default value) omitted so the rest of the
         * notification (has_unsaved_value, kind, etc.) still reaches the central
         * instead of losing the whole notification. */
        relay->notification.notification_type.setting.setting.has_value = false;
        relay->notification.notification_type.setting.setting.has_default_value = false;
        stream = pb_ostream_from_buffer(relay_notification->payload,
                                        sizeof(relay_notification->payload));
        encoded = pb_encode(&stream, cormoran_zmk_custom_settings_RelayNotification_fields, relay);
    }
    if (!encoded) {
        LOG_WRN("Failed to encode custom settings relay notification: %s", PB_GET_ERROR(&stream));
        k_mutex_unlock(&notification_buffer_lock);
        return -EIO;
    }
    relay_notification->payload_size = stream.bytes_written;
    ret = raise_zmk_custom_settings_relay_notification(*relay_notification);
#else
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    int index = custom_subsystem_index();
    if (index < 0) {
        k_mutex_unlock(&notification_buffer_lock);
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
        .arg = notification,
    };

    ret = raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
#else
    ret = 0;
#endif
#endif
    k_mutex_unlock(&notification_buffer_lock);
    return ret;
}

static bool setting_is_active(const struct zmk_custom_setting *setting) {
    return !zmk_custom_setting_is_array(setting) ||
           setting->array_index < zmk_custom_setting_array_size(setting);
}

static bool can_include_value(const struct zmk_custom_setting *setting) {
    return setting_is_active(setting) &&
           setting->confidentiality != ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE &&
           !needs_unlock(setting->read_permission);
}

static bool setting_matches_scope(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_settings_setting_scope *scope) {
    return zmk_custom_setting_matches(setting, setting_scope_custom_subsystem_id(scope),
                                      setting_scope_key(scope), setting_scope_key_prefix(scope));
}

/*
 * ZMK_CUSTOM_SETTING_FOREACH yields exactly one registered zmk_custom_setting
 * per array (the descriptor, with array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE),
 * not one per active element, but list enumeration sends one notification per
 * element (what Studio/the web UI expect), so this walk expands each array
 * descriptor into its active elements itself. setting_is_active() above reports
 * false for a bare array descriptor (ZMK_CUSTOM_SETTING_ARRAY_NONE is never
 * < any array_size), so a plain ZMK_CUSTOM_SETTING_FOREACH would silently skip
 * every array setting entirely - this callback-based walk fixes that for both
 * the "how many items match" count and the "send the Nth item" paths without
 * duplicating the expansion logic between them.
 *
 * The visitor returns false to stop iterating early (used to resume a batched
 * send at a specific flattened index).
 */
typedef bool (*list_item_visitor_t)(const struct zmk_custom_setting *item, void *user_data);

static bool for_each_list_item(const struct zmk_custom_settings_setting_scope *scope,
                               list_item_visitor_t visitor, void *user_data) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (zmk_custom_setting_is_array(setting) &&
            setting->array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
            uint32_t active_size = zmk_custom_setting_array_size(setting);
            for (uint32_t i = 0; i < active_size; i++) {
                const struct zmk_custom_setting *element = zmk_custom_setting_find_array_element(
                    setting->custom_subsystem_id, setting->array_key, i);
                if (!element || !setting_matches_scope(element, scope)) {
                    continue;
                }
                if (!visitor(element, user_data)) {
                    return false;
                }
            }
            continue;
        }

        if (!setting_is_active(setting) || !setting_matches_scope(setting, scope)) {
            continue;
        }
        if (!visitor(setting, user_data)) {
            return false;
        }
    }

    /* Keyspace slots are not reachable through ZMK_CUSTOM_SETTING_FOREACH -
     * walk every keyspace's live slots explicitly so
     * ListSettings/GetSetting/notifications still surface user-created
     * entries. A slot is never an array, so it needs no expansion step the way
     * array descriptors do above. IS_ENABLED-guarded so a
     * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n build never walks keyspace slot
     * state. */
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)) {
        ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
            for (uint32_t i = 0; i < keyspace->max_entries; i++) {
                if (!keyspace->slots[i].in_use) {
                    continue;
                }
                const struct zmk_custom_setting *slot_setting = &keyspace->slots[i].setting;
                if (!setting_matches_scope(slot_setting, scope)) {
                    continue;
                }
                if (!visitor(slot_setting, user_data)) {
                    return false;
                }
            }
        }
    }

    return true;
}

static bool list_count_visitor(const struct zmk_custom_setting *item, void *user_data) {
    ARG_UNUSED(item);
    (*(uint32_t *)user_data)++;
    return true;
}

static void list_settings_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(list_settings_work, list_settings_work_handler);
static K_MUTEX_DEFINE(list_settings_lock);
static struct zmk_custom_settings_setting_scope list_settings_scope;
static size_t list_settings_next_index;
static bool list_settings_active;
static bool list_settings_include_meta;
static bool list_settings_include_default;
static uint32_t list_settings_started_ms;

static int schedule_list_settings_work(k_timeout_t delay) {
    return k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &list_settings_work, delay);
}

struct list_send_context {
    size_t index;
    size_t next_index;
    size_t sent;
    bool include_meta;
    bool include_default;
};

static bool list_send_visitor(const struct zmk_custom_setting *item, void *user_data) {
    struct list_send_context *ctx = user_data;

    if (ctx->index++ < ctx->next_index) {
        return true;
    }

    k_mutex_lock(&list_settings_lock, K_FOREVER);
    list_settings_next_index = ctx->index;
    k_mutex_unlock(&list_settings_lock);

    int ret = raise_setting_notification(
        item,
        cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_LIST_ITEM,
        can_include_value(item), ctx->include_meta, ctx->include_default,
        ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        LOG_WRN("Failed to raise custom settings list notification: %d", ret);
    }

    /* Send a batch of items per work cycle instead of one item per delayed
     * reschedule; each item is still one notification frame. */
    if (++ctx->sent >= CONFIG_ZMK_CUSTOM_SETTINGS_LIST_NOTIFICATION_BATCH_SIZE) {
        schedule_list_settings_work(LIST_SETTINGS_NOTIFICATION_DELAY);
        return false;
    }

    return true;
}

static void list_settings_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&list_settings_lock, K_FOREVER);
    if (!list_settings_active) {
        k_mutex_unlock(&list_settings_lock);
        return;
    }
    struct zmk_custom_settings_setting_scope scope = list_settings_scope;
    size_t next_index = list_settings_next_index;
    bool include_meta = list_settings_include_meta;
    bool include_default = list_settings_include_default;
    uint32_t started_ms = list_settings_started_ms;
    k_mutex_unlock(&list_settings_lock);

    struct list_send_context ctx = {
        .next_index = next_index,
        .include_meta = include_meta,
        .include_default = include_default,
    };
    if (!for_each_list_item(&scope, list_send_visitor, &ctx)) {
        /* Stopped early because a batch was sent and the next work cycle
         * was already scheduled by list_send_visitor. */
        return;
    }

    k_mutex_lock(&list_settings_lock, K_FOREVER);
    list_settings_active = false;
    k_mutex_unlock(&list_settings_lock);
    LOG_INF("Custom settings list complete: items=%u elapsed_ms=%u", (unsigned)ctx.index,
            (uint32_t)((uint32_t)k_uptime_get() - started_ms));
}

static void schedule_list_settings(const struct zmk_custom_settings_setting_scope *scope,
                                   bool include_meta, bool include_default) {
    k_work_cancel_delayable(&list_settings_work);

    k_mutex_lock(&list_settings_lock, K_FOREVER);
    list_settings_scope = *scope;
    list_settings_next_index = 0;
    list_settings_started_ms = (uint32_t)k_uptime_get();
    list_settings_active = true;
    list_settings_include_meta = include_meta;
    list_settings_include_default = include_default;
    k_mutex_unlock(&list_settings_lock);

    LOG_DBG("settings list scheduled: source=%u meta=%d defaults=%d", setting_scope_source(scope),
            include_meta, include_default);
    schedule_list_settings_work(LIST_SETTINGS_NOTIFICATION_DELAY);
}

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int setting_ref_to_private(const cormoran_zmk_custom_settings_SettingRef *src,
                                  struct zmk_custom_settings_setting_ref *dest) {
    *dest = (struct zmk_custom_settings_setting_ref){0};

    if (src->has_custom_subsystem_index) {
        const char *identifier = custom_subsystem_identifier_for_index(src->custom_subsystem_index);
        if (!identifier) {
            return -ENOENT;
        }
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id), identifier);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source;
    }
    if (src->has_array_index) {
        dest->has_array_index = true;
        dest->array_index = src->array_index;
    }

    return 0;
}

static int setting_scope_to_private(const cormoran_zmk_custom_settings_SettingScope *src,
                                    struct zmk_custom_settings_setting_scope *dest) {
    *dest = (struct zmk_custom_settings_setting_scope){0};

    if (src->has_custom_subsystem_index) {
        const char *identifier = custom_subsystem_identifier_for_index(src->custom_subsystem_index);
        if (!identifier) {
            return -ENOENT;
        }
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id), identifier);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_key_prefix) {
        dest->has_key_prefix = true;
        copy_string(dest->key_prefix, sizeof(dest->key_prefix), src->key_prefix);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source;
    }

    return 0;
}
#endif

static int handle_private_list_settings(const struct zmk_custom_settings_setting_scope *scope,
                                        bool require_meta, bool require_default,
                                        cormoran_zmk_custom_settings_Response *resp) {
    uint32_t count = 0;

    if (!source_targets_local(setting_scope_source(scope))) {
        set_status(resp, 0, "No local settings matched source");
        return 0;
    }

    for_each_list_item(scope, list_count_visitor, &count);

    if (count > 0) {
        schedule_list_settings(scope, require_meta, require_default);
    }

    LOG_INF("settings list accepted: count=%u source=%u meta=%d defaults=%d", count,
            setting_scope_source(scope), require_meta, require_default);
    set_status(resp, count, "List started");
    return 0;
}

static int handle_list_settings(const cormoran_zmk_custom_settings_ListSettingsRequest *req,
                                cormoran_zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_scope scope;
    int ret = setting_scope_to_private(&req->scope, &scope);
    if (ret < 0) {
        return ret;
    }

    return handle_private_list_settings(&scope, req->require_meta, req->require_default, resp);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_get_setting(const struct zmk_custom_settings_setting_ref *ref,
                                      cormoran_zmk_custom_settings_Response *resp,
                                      bool include_meta, bool include_default) {
    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        return -ENOENT;
    }

    const struct zmk_custom_setting *setting = setting_for_ref(ref);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->read_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    resp->which_response_type = cormoran_zmk_custom_settings_Response_get_setting_tag;
    resp->response_type.get_setting = (cormoran_zmk_custom_settings_GetSettingResponse)
        cormoran_zmk_custom_settings_GetSettingResponse_init_zero;
    resp->response_type.get_setting.has_setting = true;
    int ret = setting_to_proto(setting, &resp->response_type.get_setting.setting, true,
                               include_meta, include_default, ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        resp->which_response_type = 0;
        return ret;
    }

    return 0;
}

static int handle_get_setting(const cormoran_zmk_custom_settings_GetSettingRequest *req,
                              cormoran_zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    return handle_private_get_setting(&private_ref, resp, req->require_meta, req->require_default);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_write_setting(const struct zmk_custom_settings_setting_ref *ref,
                                        const struct zmk_custom_setting_value *value,
                                        bool value_is_array, uint32_t array_index,
                                        uint32_t array_size,
                                        cormoran_zmk_custom_settings_SettingWriteMode write_mode,
                                        cormoran_zmk_custom_settings_Response *resp,
                                        bool value_uses_rpc_format) {
    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    struct zmk_custom_settings_setting_ref resolved_ref = *ref;
    if (value_is_array || ref->has_array_index) {
        uint32_t resolved_index = value_is_array ? array_index : ref->array_index;
        if (value_is_array && ref->has_array_index && ref->array_index != array_index) {
            return -EINVAL;
        }

        resolved_ref.has_array_index = true;
        resolved_ref.array_index = resolved_index;
    }

    const struct zmk_custom_setting *setting = setting_for_ref(&resolved_ref);
    if (!setting) {
        return -ENOENT;
    }
    /* IS_ENABLED-guarded so the zmk_custom_setting_array_max_size /
     * zmk_custom_setting_write_array_element calls below - only defined in
     * src/custom_settings_array.c when CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY is
     * enabled - are provably dead code when the feature is off (value_is_array
     * is a runtime flag decoded off the wire, so it alone cannot make that
     * provable to the compiler). */
    bool write_as_array = IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) && value_is_array;
    if (write_as_array) {
        if (array_size == 0 || array_size > zmk_custom_setting_array_max_size(setting) ||
            setting->array_index >= array_size) {
            return -EINVAL;
        }
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        write_mode == cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    struct zmk_custom_setting_value internal_value;
    int ret = 0;
    if (value_uses_rpc_format) {
        ret = zmk_custom_setting_deserialize_rpc_value(setting, value, &internal_value);
        if (ret < 0) {
            return ret;
        }
        value = &internal_value;
    }

    ret = write_as_array ? zmk_custom_setting_write_array_element(setting, value, array_size, mode)
                         : zmk_custom_setting_write(setting, value, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Setting written");
    return 0;
}

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
/* Scratch destination for decoding Request.request_type.{write_setting,
 * create_setting}.value's bytes_value/string_value (see
 * wire_setting_value_decode). Wired before pb_decode()-ing a Request in
 * custom_settings_rpc_handle_request, then read by handle_write_setting /
 * handle_create_setting / relay_request_unlock_required / request_to_relay_
 * request while processing that same decoded Request - safe to share since
 * decode happens synchronously while processing exactly one Request at a
 * time. Bounded to one RPC frame's worth of value - a bigger value still
 * needs WriteValueChunk. */
static uint8_t g_write_value_decode_buf[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
static struct bounded_decode_scratch g_write_value_decode_scratch = {
    .buf = g_write_value_decode_buf,
    .capacity = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
};
#endif

static int handle_write_setting(const cormoran_zmk_custom_settings_WriteSettingRequest *req,
                                cormoran_zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    bool value_is_array = false;
    uint32_t array_index = 0;
    uint32_t array_size = 0;
    ret = proto_to_value(&req->value, &g_write_value_decode_scratch, &value, &value_is_array,
                         &array_index, &array_size);
    if (ret < 0) {
        return ret;
    }

    return handle_private_write_setting(&private_ref, &value, value_is_array, array_index,
                                        array_size, req->mode, resp, true);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_push_back_array(const struct zmk_custom_settings_setting_ref *ref,
                                          const struct zmk_custom_setting_value *value,
                                          cormoran_zmk_custom_settings_SettingWriteMode write_mode,
                                          cormoran_zmk_custom_settings_Response *resp,
                                          bool value_uses_rpc_format) {
    /* Whole-function guard (rather than only guarding the
     * zmk_custom_setting_array_push_back call below): array_for_ref already
     * returns NULL when the feature is off, but the compiler cannot prove
     * that across the call, so without this the push_back call would still
     * need custom_settings_array.c's symbol at link time. */
    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)) {
        return -ENOTSUP;
    }

    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    const struct zmk_custom_setting *setting = array_for_ref(ref);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        write_mode == cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    struct zmk_custom_setting_value internal_value;
    int ret = 0;
    if (value_uses_rpc_format) {
        ret = zmk_custom_setting_deserialize_rpc_value(setting, value, &internal_value);
        if (ret < 0) {
            return ret;
        }
        value = &internal_value;
    }

    ret = zmk_custom_setting_array_push_back(setting, value, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Array value pushed");
    return 0;
}

static int handle_push_back_array(const cormoran_zmk_custom_settings_PushBackArrayRequest *req,
                                  cormoran_zmk_custom_settings_Response *resp) {
    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)) {
        return -ENOTSUP;
    }
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    ret = scalar_proto_to_value(&req->value, &value);
    if (ret < 0) {
        return ret;
    }

    return handle_private_push_back_array(&private_ref, &value, req->mode, resp, true);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_pop_back_array(const struct zmk_custom_settings_setting_ref *ref,
                                         cormoran_zmk_custom_settings_SettingWriteMode write_mode,
                                         cormoran_zmk_custom_settings_Response *resp) {
    /* Whole-function guard, mirroring handle_private_push_back_array - see
     * its comment. */
    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)) {
        return -ENOTSUP;
    }

    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    const struct zmk_custom_setting *setting = array_for_ref(ref);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        write_mode == cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    int ret = zmk_custom_setting_array_pop_back(setting, NULL, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Array value popped");
    return 0;
}

static int handle_pop_back_array(const cormoran_zmk_custom_settings_PopBackArrayRequest *req,
                                 cormoran_zmk_custom_settings_Response *resp) {
    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)) {
        return -ENOTSUP;
    }
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    return handle_private_pop_back_array(&private_ref, req->mode, resp);
#else
    return -ENOTSUP;
#endif
}

/*
 * CreateSetting/DeleteSetting - RPC-creatable keyspace entries (see
 * ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE in custom_settings.h). Unlike every
 * other request above, these resolve their target through
 * zmk_custom_settings_keyspace_find_for_key(subsystem, key) - a keyspace,
 * not a single struct zmk_custom_setting - since the whole point is that the
 * key does not exist as a setting yet (create) or is looked up by its
 * literal persisted key rather than a compile-time descriptor (delete).
 */

static int handle_private_create_setting(const struct zmk_custom_settings_setting_ref *ref,
                                         const struct zmk_custom_setting_value *value,
                                         cormoran_zmk_custom_settings_SettingWriteMode write_mode,
                                         cormoran_zmk_custom_settings_Response *resp,
                                         bool value_uses_rpc_format) {
    /* Everything below resolves through a
     * struct zmk_custom_setting_keyspace * (zmk_custom_settings_keyspace_find_for_key/
     * zmk_custom_setting_keyspace_create, defined in
     * custom_settings_keyspace.c) - unlike every other handler, there is no
     * live struct zmk_custom_setting to guard with zmk_custom_setting_keyspace_of(),
     * so this early return is what keeps a
     * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n build from referencing those
     * symbols. */
    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)) {
        return -ENOTSUP;
    }

    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    struct zmk_custom_setting_keyspace *keyspace = zmk_custom_settings_keyspace_find_for_key(
        setting_ref_custom_subsystem_id(ref), setting_ref_key(ref));
    if (!keyspace) {
        set_error(resp, "No keyspace registered for this key");
        return 0;
    }

    if (needs_unlock(keyspace->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        write_mode == cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    /* Deserialize against the keyspace's shared value_type/converters -
     * there is no live struct zmk_custom_setting to deserialize against yet
     * (that is exactly what this call creates), so build a throwaway
     * descriptor with just the fields zmk_custom_setting_deserialize_rpc_value
     * reads (value_type, rpc_deserializer). */
    struct zmk_custom_setting_value internal_value;
    int ret = 0;
    if (value_uses_rpc_format) {
        struct zmk_custom_setting keyspace_value_shape = {
            .value_type = keyspace->value_type,
            .rpc_deserializer = keyspace->rpc_deserializer,
        };
        ret =
            zmk_custom_setting_deserialize_rpc_value(&keyspace_value_shape, value, &internal_value);
        if (ret < 0) {
            return ret;
        }
        value = &internal_value;
    }

    const struct zmk_custom_setting *created = NULL;
    ret = zmk_custom_setting_keyspace_create(keyspace, setting_ref_key(ref), value, mode, &created);
    if (ret == -ENOSPC) {
        set_error(resp, "Keyspace is full");
        return 0;
    }
    if (ret == -EEXIST) {
        set_error(resp, "Key already exists");
        return 0;
    }
    if (ret == -ENAMETOOLONG) {
        set_error(resp, "Key is too long for this keyspace");
        return 0;
    }
    if (ret < 0) {
        return ret;
    }

    /* Skip the synchronous notification when this create came through the RPC
     * entry point: its RPC response already confirms the create to the
     * single-connection Studio client, so a concurrent notification would only
     * starve that response on the shared transport. A non-RPC create keeps
     * notifying. */
    if (created && !notify_suppressed()) {
        raise_setting_notification(
            created,
            cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_VALUE_UPDATED,
            can_include_value(created), false, false, ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    }

    set_status(resp, 1, "Setting created");
    return 0;
}

static int handle_create_setting(const cormoran_zmk_custom_settings_CreateSettingRequest *req,
                                 cormoran_zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    bool value_is_array = false;
    uint32_t array_index = 0;
    uint32_t array_size = 0;
    ret = proto_to_value(&req->value, &g_write_value_decode_scratch, &value, &value_is_array,
                         &array_index, &array_size);
    if (ret < 0) {
        return ret;
    }
    if (value_is_array) {
        return -EINVAL;
    }

    return handle_private_create_setting(&private_ref, &value, req->mode, resp, true);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_delete_setting(const struct zmk_custom_settings_setting_ref *ref,
                                         cormoran_zmk_custom_settings_Response *resp) {
    /* See the matching guard/comment in handle_private_create_setting. */
    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)) {
        return -ENOTSUP;
    }

    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    struct zmk_custom_setting_keyspace *keyspace = zmk_custom_settings_keyspace_find_for_key(
        setting_ref_custom_subsystem_id(ref), setting_ref_key(ref));
    if (!keyspace) {
        return -ENOENT;
    }

    if (needs_unlock(keyspace->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    /* Notify before releasing the slot: raise_setting_notification (the
     * same helper list/create/write use, so encoding/relay behavior stays
     * consistent) reads the setting's key straight out of the slot's own
     * key buffer, which zmk_custom_setting_keyspace_delete is about to hand
     * back to the pool for reuse. */
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_keyspace_find(keyspace, setting_ref_key(ref));
    /* As with create above: an RPC-originated delete is already confirmed by
     * its RPC response, so skip the concurrent notification that would starve
     * it on the shared transport. A non-RPC delete keeps notifying. */
    if (setting && !notify_suppressed()) {
        raise_setting_notification(
            setting,
            cormoran_zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_DISCARDED,
            false, false, false, ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    }

    int ret = zmk_custom_setting_keyspace_delete(keyspace, setting_ref_key(ref));
    if (ret == -ENOENT) {
        set_error(resp, "Key does not exist");
        return 0;
    }
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Setting deleted");
    return 0;
}

static int handle_delete_setting(const cormoran_zmk_custom_settings_DeleteSettingRequest *req,
                                 cormoran_zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    return handle_private_delete_setting(&private_ref, resp);
#else
    return -ENOTSUP;
#endif
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_CHUNKED_RPC) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
struct zmk_custom_settings_chunk_session {
    bool active;
    const struct zmk_custom_setting *setting;
    uint32_t total_size;
    uint32_t received;
    cormoran_zmk_custom_settings_SettingWriteMode mode;
    uint8_t buffer[CONFIG_ZMK_CUSTOM_SETTINGS_CHUNK_STAGING_SIZE];
};

static K_MUTEX_DEFINE(chunk_session_lock);
static struct zmk_custom_settings_chunk_session chunk_session;

static bool chunk_value_type_supported(const struct zmk_custom_setting *setting) {
    enum zmk_custom_setting_value_type type = presented_value_type(setting);
    return type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
           type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
}

static void reset_chunk_session_locked(void) {
    chunk_session = (struct zmk_custom_settings_chunk_session){0};
}

static int
handle_private_write_value_chunk(const struct zmk_custom_settings_setting_ref *ref,
                                 uint32_t total_size, uint32_t offset, const uint8_t *data,
                                 size_t data_size, bool commit,
                                 cormoran_zmk_custom_settings_SettingWriteMode write_mode,
                                 cormoran_zmk_custom_settings_Response *resp) {
    if (!ref->has_key) {
        return -EINVAL;
    }
    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    const struct zmk_custom_setting *setting = setting_for_ref(ref);
    if (!setting) {
        return -ENOENT;
    }
    if (!chunk_value_type_supported(setting)) {
        return -ENOTSUP;
    }
    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    k_mutex_lock(&chunk_session_lock, K_FOREVER);

    if (offset == 0) {
        if (total_size > sizeof(chunk_session.buffer)) {
            k_mutex_unlock(&chunk_session_lock);
            return -EMSGSIZE;
        }
        reset_chunk_session_locked();
        chunk_session.active = true;
        chunk_session.setting = setting;
        chunk_session.total_size = total_size;
        chunk_session.mode = write_mode;
    } else if (!chunk_session.active || chunk_session.setting != setting ||
               offset != chunk_session.received) {
        /* Out-of-order chunk, or a chunk for a different setting than the
         * transfer that is currently in progress. */
        k_mutex_unlock(&chunk_session_lock);
        return -EINVAL;
    }

    if (data_size > chunk_session.total_size - chunk_session.received) {
        reset_chunk_session_locked();
        k_mutex_unlock(&chunk_session_lock);
        return -EMSGSIZE;
    }

    memcpy(&chunk_session.buffer[chunk_session.received], data, data_size);
    chunk_session.received += data_size;

    if (!commit) {
        set_status(resp, chunk_session.received, "Chunk received");
        k_mutex_unlock(&chunk_session_lock);
        return 0;
    }

    if (chunk_session.received != chunk_session.total_size) {
        reset_chunk_session_locked();
        k_mutex_unlock(&chunk_session_lock);
        return -EINVAL;
    }

    enum zmk_custom_setting_write_mode mode =
        chunk_session.mode ==
                cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    int ret;
    if (chunk_session.total_size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        /* Large value: write the assembled payload straight to the setting's
         * large store. RPC deserializer converters are bounded by the fixed
         * carrier and are not applied on this path. The write is done while
         * still holding chunk_session_lock (consistent lock order:
         * chunk_session_lock is always taken before custom_settings_lock), then
         * the session is released. */
        ret = zmk_custom_setting_write_bytes(setting, chunk_session.buffer,
                                             chunk_session.total_size, mode);
        reset_chunk_session_locked();
        k_mutex_unlock(&chunk_session_lock);
        if (ret < 0) {
            return ret;
        }

        set_status(resp, 1, "Value written from chunks");
        return 0;
    }

    struct zmk_custom_setting_value rpc_value = {.type = presented_value_type(setting)};
    if (rpc_value.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES) {
        rpc_value.size = chunk_session.total_size;
        memcpy(rpc_value.bytes_value, chunk_session.buffer, chunk_session.total_size);
    } else {
        size_t len = MIN(chunk_session.total_size, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        memcpy(rpc_value.string_value, chunk_session.buffer, len);
        rpc_value.string_value[len] = '\0';
        rpc_value.size = len;
    }

    struct zmk_custom_setting_value internal_value;
    ret = zmk_custom_setting_deserialize_rpc_value(setting, &rpc_value, &internal_value);
    reset_chunk_session_locked();
    k_mutex_unlock(&chunk_session_lock);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(setting, &internal_value, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Value written from chunks");
    return 0;
}
#endif

static int handle_write_value_chunk(const cormoran_zmk_custom_settings_WriteValueChunkRequest *req,
                                    cormoran_zmk_custom_settings_Response *resp) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_CHUNKED_RPC) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    return handle_private_write_value_chunk(&private_ref, req->total_size, req->offset,
                                            req->data.bytes, req->data.size, req->commit, req->mode,
                                            resp);
#else
    return -ENOTSUP;
#endif
}

static bool scope_has_permission(const struct zmk_custom_settings_setting_scope *scope,
                                 enum zmk_custom_setting_permission permission,
                                 bool read_permission) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (setting_matches_scope(setting, scope) &&
            (read_permission ? setting->read_permission : setting->write_permission) ==
                permission) {
            return true;
        }
    }

    return false;
}

static bool scope_write_unlock_required(const struct zmk_custom_settings_setting_scope *scope) {
    return !studio_is_unlocked() &&
           scope_has_permission(scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
}

static int handle_private_scope_mutation(const struct zmk_custom_settings_setting_scope *scope,
                                         cormoran_zmk_custom_settings_Response *resp,
                                         const char *message,
                                         int (*callback)(const char *, const char *, const char *,
                                                         uint32_t *)) {
    if (!source_targets_local(setting_scope_source(scope))) {
        set_status(resp, 0, "No local settings matched source");
        return 0;
    }

    if (scope_write_unlock_required(scope)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    uint32_t count = 0;
    int ret = callback(setting_scope_custom_subsystem_id(scope), setting_scope_key(scope),
                       setting_scope_key_prefix(scope), &count);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, count, message);
    return 0;
}

static int handle_scope_mutation(const cormoran_zmk_custom_settings_SettingScope *scope,
                                 cormoran_zmk_custom_settings_Response *resp, const char *message,
                                 int (*callback)(const char *, const char *, const char *,
                                                 uint32_t *)) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_scope private_scope;
    int ret = setting_scope_to_private(scope, &private_scope);
    if (ret < 0) {
        return ret;
    }

    return handle_private_scope_mutation(&private_scope, resp, message, callback);
#else
    return -ENOTSUP;
#endif
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static bool should_relay_to_peripherals(const cormoran_zmk_custom_settings_Request *req) {
    switch (req->which_request_type) {
    case cormoran_zmk_custom_settings_Request_list_settings_tag:
        return source_targets_peripherals(scope_source(&req->request_type.list_settings.scope));
    case cormoran_zmk_custom_settings_Request_save_settings_tag:
        return source_targets_peripherals(scope_source(&req->request_type.save_settings.scope));
    case cormoran_zmk_custom_settings_Request_discard_settings_tag:
        return source_targets_peripherals(scope_source(&req->request_type.discard_settings.scope));
    case cormoran_zmk_custom_settings_Request_reset_settings_tag:
        return source_targets_peripherals(scope_source(&req->request_type.reset_settings.scope));
    case cormoran_zmk_custom_settings_Request_write_setting_tag:
        return source_targets_peripherals(ref_source(&req->request_type.write_setting.setting));
    case cormoran_zmk_custom_settings_Request_push_back_array_tag:
        return source_targets_peripherals(ref_source(&req->request_type.push_back_array.setting));
    case cormoran_zmk_custom_settings_Request_pop_back_array_tag:
        return source_targets_peripherals(ref_source(&req->request_type.pop_back_array.setting));
    default:
        return false;
    }
}

static bool relay_request_unlock_required(const cormoran_zmk_custom_settings_Request *req) {
    if (studio_is_unlocked()) {
        return false;
    }

    switch (req->which_request_type) {
    case cormoran_zmk_custom_settings_Request_list_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.list_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, true);
    }
    case cormoran_zmk_custom_settings_Request_write_setting_tag: {
        struct zmk_custom_settings_setting_ref ref;
        if (setting_ref_to_private(&req->request_type.write_setting.setting, &ref) < 0) {
            return false;
        }

        struct zmk_custom_setting_value value;
        bool value_is_array = false;
        uint32_t array_index = 0;
        uint32_t array_size = 0;
        if (proto_to_value(&req->request_type.write_setting.value, &g_write_value_decode_scratch,
                           &value, &value_is_array, &array_index, &array_size) < 0) {
            return false;
        }
        if (value_is_array) {
            ref.has_array_index = true;
            ref.array_index = array_index;
        }

        const struct zmk_custom_setting *setting = setting_for_ref(&ref);
        return setting && setting->write_permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE;
    }
    case cormoran_zmk_custom_settings_Request_push_back_array_tag: {
        struct zmk_custom_settings_setting_ref ref;
        if (setting_ref_to_private(&req->request_type.push_back_array.setting, &ref) < 0) {
            return false;
        }

        const struct zmk_custom_setting *setting = array_for_ref(&ref);
        return setting && setting->write_permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE;
    }
    case cormoran_zmk_custom_settings_Request_pop_back_array_tag: {
        struct zmk_custom_settings_setting_ref ref;
        if (setting_ref_to_private(&req->request_type.pop_back_array.setting, &ref) < 0) {
            return false;
        }

        const struct zmk_custom_setting *setting = array_for_ref(&ref);
        return setting && setting->write_permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE;
    }
    case cormoran_zmk_custom_settings_Request_save_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.save_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
    }
    case cormoran_zmk_custom_settings_Request_discard_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.discard_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
    }
    case cormoran_zmk_custom_settings_Request_reset_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.reset_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
    }
    default:
        return false;
    }
}
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int public_ref_to_relay(const cormoran_zmk_custom_settings_SettingRef *src,
                               cormoran_zmk_custom_settings_RelaySettingRef *dest) {
    *dest = (cormoran_zmk_custom_settings_RelaySettingRef)
        cormoran_zmk_custom_settings_RelaySettingRef_init_zero;

    if (src->has_custom_subsystem_index) {
        const char *identifier = custom_subsystem_identifier_for_index(src->custom_subsystem_index);
        if (!identifier) {
            return -ENOENT;
        }
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id), identifier);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source == ZMK_CUSTOM_SETTING_SOURCE_ALL
                           ? ZMK_CUSTOM_SETTING_SOURCE_ALL
                           : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
    }
    if (src->has_array_index) {
        dest->has_array_index = true;
        dest->array_index = src->array_index;
    }

    return 0;
}

static int public_scope_to_relay(const cormoran_zmk_custom_settings_SettingScope *src,
                                 cormoran_zmk_custom_settings_RelaySettingScope *dest) {
    *dest = (cormoran_zmk_custom_settings_RelaySettingScope)
        cormoran_zmk_custom_settings_RelaySettingScope_init_zero;

    if (src->has_custom_subsystem_index) {
        const char *identifier = custom_subsystem_identifier_for_index(src->custom_subsystem_index);
        if (!identifier) {
            return -ENOENT;
        }
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id), identifier);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_key_prefix) {
        dest->has_key_prefix = true;
        copy_string(dest->key_prefix, sizeof(dest->key_prefix), src->key_prefix);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source == ZMK_CUSTOM_SETTING_SOURCE_ALL
                           ? ZMK_CUSTOM_SETTING_SOURCE_ALL
                           : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
    }

    return 0;
}

static int request_to_relay_request(const cormoran_zmk_custom_settings_Request *src,
                                    struct zmk_custom_settings_relay_request *dest) {
    *dest = (struct zmk_custom_settings_relay_request){
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    };

    cormoran_zmk_custom_settings_RelayRequest relay =
        cormoran_zmk_custom_settings_RelayRequest_init_zero;
    int ret;

    switch (src->which_request_type) {
    case cormoran_zmk_custom_settings_Request_list_settings_tag:
        relay.which_request_type = cormoran_zmk_custom_settings_RelayRequest_list_settings_tag;
        relay.request_type.list_settings.has_scope = true;
        ret = public_scope_to_relay(&src->request_type.list_settings.scope,
                                    &relay.request_type.list_settings.scope);
        if (ret < 0) {
            return ret;
        }
        relay.request_type.list_settings.require_meta =
            src->request_type.list_settings.require_meta;
        relay.request_type.list_settings.require_default =
            src->request_type.list_settings.require_default;
        break;
    case cormoran_zmk_custom_settings_Request_write_setting_tag:
        relay.which_request_type = cormoran_zmk_custom_settings_RelayRequest_write_setting_tag;
        relay.request_type.write_setting.has_setting = true;
        ret = public_ref_to_relay(&src->request_type.write_setting.setting,
                                  &relay.request_type.write_setting.setting);
        if (ret < 0) {
            return ret;
        }
        relay.request_type.write_setting.has_value = true;
        relay.request_type.write_setting.value = src->request_type.write_setting.value;
        /* src's bytes_value/string_value (if that is the active oneof arm)
         * is a callback still wired for DECODE (see
         * g_write_value_decode_scratch) - re-target the copy to stream the
         * same already-decoded bytes back out for this re-encode. A no-op
         * for every other which_value_type. */
        retarget_value_to_encode_scratch(&relay.request_type.write_setting.value,
                                         &g_write_value_decode_scratch);
        relay.request_type.write_setting.mode = src->request_type.write_setting.mode;
        break;
    case cormoran_zmk_custom_settings_Request_push_back_array_tag:
        relay.which_request_type = cormoran_zmk_custom_settings_RelayRequest_push_back_array_tag;
        relay.request_type.push_back_array.has_setting = true;
        ret = public_ref_to_relay(&src->request_type.push_back_array.setting,
                                  &relay.request_type.push_back_array.setting);
        if (ret < 0) {
            return ret;
        }
        relay.request_type.push_back_array.has_value = true;
        relay.request_type.push_back_array.value = src->request_type.push_back_array.value;
        relay.request_type.push_back_array.mode = src->request_type.push_back_array.mode;
        break;
    case cormoran_zmk_custom_settings_Request_pop_back_array_tag:
        relay.which_request_type = cormoran_zmk_custom_settings_RelayRequest_pop_back_array_tag;
        relay.request_type.pop_back_array.has_setting = true;
        ret = public_ref_to_relay(&src->request_type.pop_back_array.setting,
                                  &relay.request_type.pop_back_array.setting);
        if (ret < 0) {
            return ret;
        }
        relay.request_type.pop_back_array.mode = src->request_type.pop_back_array.mode;
        break;
    case cormoran_zmk_custom_settings_Request_save_settings_tag:
        relay.which_request_type = cormoran_zmk_custom_settings_RelayRequest_save_settings_tag;
        relay.request_type.save_settings.has_scope = true;
        ret = public_scope_to_relay(&src->request_type.save_settings.scope,
                                    &relay.request_type.save_settings.scope);
        if (ret < 0) {
            return ret;
        }
        break;
    case cormoran_zmk_custom_settings_Request_discard_settings_tag:
        relay.which_request_type = cormoran_zmk_custom_settings_RelayRequest_discard_settings_tag;
        relay.request_type.discard_settings.has_scope = true;
        ret = public_scope_to_relay(&src->request_type.discard_settings.scope,
                                    &relay.request_type.discard_settings.scope);
        if (ret < 0) {
            return ret;
        }
        break;
    case cormoran_zmk_custom_settings_Request_reset_settings_tag:
        relay.which_request_type = cormoran_zmk_custom_settings_RelayRequest_reset_settings_tag;
        relay.request_type.reset_settings.has_scope = true;
        ret = public_scope_to_relay(&src->request_type.reset_settings.scope,
                                    &relay.request_type.reset_settings.scope);
        if (ret < 0) {
            return ret;
        }
        break;
    default:
        return -ENOTSUP;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(dest->payload, sizeof(dest->payload));
    if (!pb_encode(&stream, cormoran_zmk_custom_settings_RelayRequest_fields, &relay)) {
        LOG_WRN("Failed to encode custom settings relay request: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    if (stream.bytes_written > sizeof(dest->payload)) {
        return -EMSGSIZE;
    }
    dest->payload_size = stream.bytes_written;
    return 0;
}

static int relay_request_to_peripherals(const cormoran_zmk_custom_settings_Request *req) {
    struct zmk_custom_settings_relay_request relay_request;

    int ret = request_to_relay_request(req, &relay_request);
    if (ret < 0) {
        return ret;
    }

    return raise_zmk_custom_settings_relay_request(relay_request);
}

static void list_settings_relay_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(list_settings_relay_work, list_settings_relay_work_handler);
static K_MUTEX_DEFINE(list_settings_relay_lock);
static struct zmk_custom_settings_relay_request list_settings_relay_request;
static bool list_settings_relay_active;

static void list_settings_relay_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&list_settings_relay_lock, K_FOREVER);
    if (!list_settings_relay_active) {
        k_mutex_unlock(&list_settings_relay_lock);
        return;
    }
    struct zmk_custom_settings_relay_request relay_request = list_settings_relay_request;
    list_settings_relay_active = false;
    k_mutex_unlock(&list_settings_relay_lock);

    LOG_DBG("Custom settings relaying list request to peripherals");
    int ret = raise_zmk_custom_settings_relay_request(relay_request);
    if (ret < 0) {
        LOG_WRN("Failed to relay custom settings list request: %d", ret);
    }
}

static int
schedule_list_settings_relay_to_peripherals(const cormoran_zmk_custom_settings_Request *req) {
    struct zmk_custom_settings_relay_request relay_request;

    int ret = request_to_relay_request(req, &relay_request);
    if (ret < 0) {
        return ret;
    }

    k_work_cancel_delayable(&list_settings_relay_work);
    k_mutex_lock(&list_settings_relay_lock, K_FOREVER);
    list_settings_relay_request = relay_request;
    list_settings_relay_active = true;
    k_mutex_unlock(&list_settings_relay_lock);

    LOG_DBG("Custom settings scheduled list relay to peripherals");
    k_work_schedule(&list_settings_relay_work, LIST_SETTINGS_RELAY_DELAY);
    return 0;
}
#endif

static int process_request(const cormoran_zmk_custom_settings_Request *req,
                           cormoran_zmk_custom_settings_Response *resp) {
    switch (req->which_request_type) {
    case cormoran_zmk_custom_settings_Request_list_settings_tag:
        return handle_list_settings(&req->request_type.list_settings, resp);
    case cormoran_zmk_custom_settings_Request_get_setting_tag:
        return handle_get_setting(&req->request_type.get_setting, resp);
    case cormoran_zmk_custom_settings_Request_write_setting_tag:
        return handle_write_setting(&req->request_type.write_setting, resp);
    case cormoran_zmk_custom_settings_Request_push_back_array_tag:
        return IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
                   ? handle_push_back_array(&req->request_type.push_back_array, resp)
                   : -ENOTSUP;
    case cormoran_zmk_custom_settings_Request_pop_back_array_tag:
        return IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
                   ? handle_pop_back_array(&req->request_type.pop_back_array, resp)
                   : -ENOTSUP;
    case cormoran_zmk_custom_settings_Request_write_value_chunk_tag:
        return handle_write_value_chunk(&req->request_type.write_value_chunk, resp);
    case cormoran_zmk_custom_settings_Request_create_setting_tag:
        return IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
                   ? handle_create_setting(&req->request_type.create_setting, resp)
                   : -ENOTSUP;
    case cormoran_zmk_custom_settings_Request_delete_setting_tag:
        return IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
                   ? handle_delete_setting(&req->request_type.delete_setting, resp)
                   : -ENOTSUP;
    case cormoran_zmk_custom_settings_Request_save_settings_tag:
        return handle_scope_mutation(&req->request_type.save_settings.scope, resp, "Settings saved",
                                     zmk_custom_settings_save_scope);
    case cormoran_zmk_custom_settings_Request_discard_settings_tag:
        return handle_scope_mutation(&req->request_type.discard_settings.scope, resp,
                                     "Settings discarded", zmk_custom_settings_discard_scope);
    case cormoran_zmk_custom_settings_Request_reset_settings_tag:
        return handle_scope_mutation(&req->request_type.reset_settings.scope, resp,
                                     "Settings reset", zmk_custom_settings_reset_scope);
    default:
        return -ENOTSUP;
    }
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
static void relay_ref_to_private(const cormoran_zmk_custom_settings_RelaySettingRef *src,
                                 struct zmk_custom_settings_setting_ref *dest) {
    *dest = (struct zmk_custom_settings_setting_ref){0};

    if (src->has_custom_subsystem_id) {
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id),
                    src->custom_subsystem_id);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source;
    }
    if (src->has_array_index) {
        dest->has_array_index = true;
        dest->array_index = src->array_index;
    }
}

static void relay_scope_to_private(const cormoran_zmk_custom_settings_RelaySettingScope *src,
                                   struct zmk_custom_settings_setting_scope *dest) {
    *dest = (struct zmk_custom_settings_setting_scope){0};

    if (src->has_custom_subsystem_id) {
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id),
                    src->custom_subsystem_id);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_key_prefix) {
        dest->has_key_prefix = true;
        copy_string(dest->key_prefix, sizeof(dest->key_prefix), src->key_prefix);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source;
    }
}

static K_MUTEX_DEFINE(relay_request_decode_lock);
static cormoran_zmk_custom_settings_RelayRequest relay_request_decode_buffer;
static cormoran_zmk_custom_settings_Response relay_request_response_buffer;
/* Separate from g_write_value_decode_scratch: this decodes a RelayRequest
 * (forwarded from central to a peripheral), a different pb_decode() call at
 * a different time than the local Studio RPC Request decode. */
static uint8_t relay_request_write_value_decode_buf[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
static struct bounded_decode_scratch relay_request_write_value_decode_scratch = {
    .buf = relay_request_write_value_decode_buf,
    .capacity = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
};

/* RelayRequest counterpart of request_arm_wire_precallback (nanopb
 * submsg_callback option on RelayRequest): wires the write_setting arm's
 * inner SettingValue payload callback once the arm's tag is known. */
static bool relay_request_arm_wire_precallback(pb_istream_t *stream, const pb_field_t *field,
                                               void **arg) {
    (void)stream;
    struct bounded_decode_scratch *scratch = *arg;
    if (field->tag == cormoran_zmk_custom_settings_RelayRequest_write_setting_tag) {
        cormoran_zmk_custom_settings_RelayWriteSettingRequest *req = field->pData;
        wire_setting_value_decode(&req->value, scratch);
    }
    return true;
}

static int process_relay_request(const struct zmk_custom_settings_relay_request *req,
                                 cormoran_zmk_custom_settings_Response *resp) {
    k_mutex_lock(&relay_request_decode_lock, K_FOREVER);

    cormoran_zmk_custom_settings_RelayRequest *decoded_req = &relay_request_decode_buffer;
    *decoded_req = (cormoran_zmk_custom_settings_RelayRequest)
        cormoran_zmk_custom_settings_RelayRequest_init_zero;
    /* Wired via the message-level oneof precallback, not in advance - see
     * request_arm_wire_precallback's comment (nanopb zeroes the selected
     * arm during decode). */
    relay_request_write_value_decode_scratch.size = 0;
    decoded_req->cb_request_type.funcs.decode = relay_request_arm_wire_precallback;
    decoded_req->cb_request_type.arg = &relay_request_write_value_decode_scratch;
    pb_istream_t req_stream = pb_istream_from_buffer(req->payload, req->payload_size);
    if (!pb_decode(&req_stream, cormoran_zmk_custom_settings_RelayRequest_fields, decoded_req)) {
        LOG_WRN("Failed to decode relayed custom settings request: %s", PB_GET_ERROR(&req_stream));
        k_mutex_unlock(&relay_request_decode_lock);
        return -EIO;
    }

    int ret = 0;
    switch (decoded_req->which_request_type) {
    case cormoran_zmk_custom_settings_RelayRequest_list_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        relay_scope_to_private(&decoded_req->request_type.list_settings.scope, &scope);
        ret = handle_private_list_settings(
            &scope, decoded_req->request_type.list_settings.require_meta,
            decoded_req->request_type.list_settings.require_default, resp);
        break;
    }
    case cormoran_zmk_custom_settings_RelayRequest_write_setting_tag: {
        struct zmk_custom_settings_setting_ref ref;
        relay_ref_to_private(&decoded_req->request_type.write_setting.setting, &ref);

        struct zmk_custom_setting_value value;
        bool value_is_array = false;
        uint32_t array_index = 0;
        uint32_t array_size = 0;
        ret = proto_to_value(&decoded_req->request_type.write_setting.value,
                             &relay_request_write_value_decode_scratch, &value, &value_is_array,
                             &array_index, &array_size);
        if (ret < 0) {
            break;
        }
        ret =
            handle_private_write_setting(&ref, &value, value_is_array, array_index, array_size,
                                         decoded_req->request_type.write_setting.mode, resp, true);
        break;
    }
    case cormoran_zmk_custom_settings_RelayRequest_push_back_array_tag: {
        struct zmk_custom_settings_setting_ref ref;
        relay_ref_to_private(&decoded_req->request_type.push_back_array.setting, &ref);

        struct zmk_custom_setting_value value;
        ret = scalar_proto_to_value(&decoded_req->request_type.push_back_array.value, &value);
        if (ret < 0) {
            break;
        }
        ret = handle_private_push_back_array(
            &ref, &value, decoded_req->request_type.push_back_array.mode, resp, true);
        break;
    }
    case cormoran_zmk_custom_settings_RelayRequest_pop_back_array_tag: {
        struct zmk_custom_settings_setting_ref ref;
        relay_ref_to_private(&decoded_req->request_type.pop_back_array.setting, &ref);
        ret = handle_private_pop_back_array(&ref, decoded_req->request_type.pop_back_array.mode,
                                            resp);
        break;
    }
    case cormoran_zmk_custom_settings_RelayRequest_save_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        relay_scope_to_private(&decoded_req->request_type.save_settings.scope, &scope);
        ret = handle_private_scope_mutation(&scope, resp, "Settings saved",
                                            zmk_custom_settings_save_scope);
        break;
    }
    case cormoran_zmk_custom_settings_RelayRequest_discard_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        relay_scope_to_private(&decoded_req->request_type.discard_settings.scope, &scope);
        ret = handle_private_scope_mutation(&scope, resp, "Settings discarded",
                                            zmk_custom_settings_discard_scope);
        break;
    }
    case cormoran_zmk_custom_settings_RelayRequest_reset_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        relay_scope_to_private(&decoded_req->request_type.reset_settings.scope, &scope);
        ret = handle_private_scope_mutation(&scope, resp, "Settings reset",
                                            zmk_custom_settings_reset_scope);
        break;
    }
    default:
        ret = -ENOTSUP;
        break;
    }

    k_mutex_unlock(&relay_request_decode_lock);
    return ret;
}

static void relay_request_work_handler(struct k_work *work);

K_MSGQ_DEFINE(relay_request_msgq, sizeof(struct zmk_custom_settings_relay_request),
              CONFIG_ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_QUEUE_SIZE, 4);
K_WORK_DEFINE(relay_request_work, relay_request_work_handler);

static void relay_request_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct zmk_custom_settings_relay_request req;
    while (k_msgq_get(&relay_request_msgq, &req, K_NO_WAIT) == 0) {
        relay_request_response_buffer =
            (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
        int ret = process_relay_request(&req, &relay_request_response_buffer);
        if (ret < 0) {
            LOG_WRN("Relayed custom settings request failed: %d", ret);
        }
    }
}
#endif

struct zmk_custom_settings_notification_request {
    const struct zmk_custom_setting *setting;
    cormoran_zmk_custom_settings_SettingNotificationKind kind;
    uint32_t source;
};

static void notification_work_handler(struct k_work *work);

K_MSGQ_DEFINE(notification_msgq, sizeof(struct zmk_custom_settings_notification_request),
              CONFIG_ZMK_CUSTOM_SETTINGS_NOTIFICATION_QUEUE_SIZE, 4);
K_WORK_DEFINE(notification_work, notification_work_handler);

static void notification_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct zmk_custom_settings_notification_request request;
    while (k_msgq_get(&notification_msgq, &request, K_NO_WAIT) == 0) {
        int ret = raise_setting_notification(request.setting, request.kind,
                                             can_include_value(request.setting), false, false,
                                             request.source);
        if (ret < 0) {
            LOG_WRN("Failed to raise custom settings notification: %d", ret);
        }
    }
}

static int setting_changed_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *ev = as_zmk_custom_setting_changed(eh);
    if (!ev || !ev->setting) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* An RPC-originated mutation already reports success via its RPC response;
     * queuing a redundant notification here would starve that response on the
     * single-connection Studio transport (see notify_suppress_depth above).
     * A call-scoped quiet event explicitly omits its Studio notification while
     * still reaching core listeners. The legacy global RPC bracket is retained;
     * it does not isolate concurrent non-RPC writers. */
    if (ev->suppress_studio_notification || notify_suppressed()) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST)
    /* Count every non-suppressed notification at the point it is about to be
     * submitted - before k_msgq_put, which can transiently fail (e.g. a full
     * queue at SYS_INIT before the low-prio workqueue has drained it). The
     * self-test only needs to know suppression let the notification through,
     * not whether the queue happened to have room. */
    notify_submit_count_for_test++;
#endif

    struct zmk_custom_settings_notification_request request = {
        .setting = ev->setting,
        .kind = proto_notification_kind(ev->kind),
        .source = ev->source,
    };
    int ret = k_msgq_put(&notification_msgq, &request, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("Failed to queue custom settings notification: %d", ret);
        return ZMK_EV_EVENT_BUBBLE;
    }

    ret = k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &notification_work);
    if (ret < 0) {
        LOG_WRN("Failed to submit custom settings notification work: %d", ret);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_settings_studio, setting_changed_listener);
ZMK_SUBSCRIPTION(custom_settings_studio, zmk_custom_setting_changed);

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
/* Decode a wire-format Request the one true way: the inner SettingValue
 * payload callback is wired from the message-level oneof precallback, NOT
 * before pb_decode() - nanopb zeroes the selected request_type arm during
 * decode, which would wipe an advance wiring (see
 * request_arm_wire_precallback). Shared by the RPC entry point below and the
 * decode-regression self-test so the test exercises the production path. */
static bool decode_custom_settings_request(const uint8_t *buf, size_t size,
                                           cormoran_zmk_custom_settings_Request *req) {
    *req = (cormoran_zmk_custom_settings_Request)cormoran_zmk_custom_settings_Request_init_zero;
    g_write_value_decode_scratch.size = 0;
    req->cb_request_type.funcs.decode = request_arm_wire_precallback;
    req->cb_request_type.arg = &g_write_value_decode_scratch;
    pb_istream_t req_stream = pb_istream_from_buffer(buf, size);
    if (!pb_decode(&req_stream, cormoran_zmk_custom_settings_Request_fields, req)) {
        LOG_WRN("Failed to decode custom settings request: %s", PB_GET_ERROR(&req_stream));
        return false;
    }
    return true;
}

static bool custom_settings_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                               pb_callback_t *encode_response) {
    cormoran_zmk_custom_settings_Response *resp = ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(
        cormoran_custom_settings, encode_response);

    cormoran_zmk_custom_settings_Request req;
    if (!decode_custom_settings_request(raw_request->payload.bytes, raw_request->payload.size,
                                        &req)) {
        set_error(resp, "Failed to decode request");
        return true;
    }
    LOG_DBG("Custom settings RPC request: type=%u payload_size=%u", req.which_request_type,
            (uint32_t)raw_request->payload.size);

    /* Suppress the self-notification these mutations raise: the RPC response
     * built below already tells the single-connection Studio client the
     * operation succeeded, and a concurrent notification would starve that
     * response on the shared USB CDC transport. Only local dispatch is
     * bracketed - the split relay that follows must keep notifying so
     * peripheral changes still propagate back to the central. */
    notify_suppress_begin();
    int ret = process_request(&req, resp);
    notify_suppress_end();
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* A peripheral has no Studio lock of its own and runs every relayed
     * request as unlocked, while this half can only check the permissions of
     * settings it registers itself. So nothing is relayed while locked. */
    if (ret == 0 && should_relay_to_peripherals(&req)) {
        if (studio_is_unlocked() && !relay_request_unlock_required(&req)) {
            if (req.which_request_type == cormoran_zmk_custom_settings_Request_list_settings_tag) {
                ret = schedule_list_settings_relay_to_peripherals(&req);
            } else {
                ret = relay_request_to_peripherals(&req);
            }
        } else {
            LOG_INF("Custom settings skipped peripheral relay: Studio is locked");
        }
    }
#endif
    LOG_DBG("Custom settings RPC request processed: ret=%d response_type=%u", ret,
            resp->which_response_type);

    if (ret < 0) {
        LOG_WRN("Custom settings request failed: %d", ret);
        switch (ret) {
        case -ENOENT:
            set_error(resp, "Setting not found");
            break;
        case -EINVAL:
            set_error(resp, "Invalid request");
            break;
        case -ERANGE:
            set_error(resp, "Value out of range");
            break;
        default:
            set_error(resp, "Failed to process request");
            break;
        }
    }

    return true;
}

/*
 * Participate in ZMK's official factory reset. ZMK Studio's core
 * `reset_settings` RPC iterates every subsystem registered via
 * ZMK_RPC_SUBSYSTEM_SETTINGS_RESET and calls its callback, so registering here
 * makes a Studio "reset settings" wipe all custom settings too - not just the
 * keymap and other built-in subsystems.
 */
static int custom_settings_reset(void) {
    /* Reset every local custom setting (all subsystems, all keys, including
     * keyspace entries): delete the persisted records and revert to defaults.
     *
     * ZMK's core reset_settings handler invokes this on the Studio RPC thread,
     * bypassing our own dispatch's notify-suppress bracket, so suppress here
     * too: a self-notification per setting would starve the RPC response on
     * the shared transport and overflow the RPC thread stack (see
     * zmk_custom_settings_notify_suppress_begin). */
    notify_suppress_begin();
    int ret = zmk_custom_settings_reset_scope(NULL, NULL, NULL, NULL);
    notify_suppress_end();
    if (ret < 0) {
        LOG_ERR("Failed to reset custom settings: %d", ret);
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Also wipe every split peripheral's custom settings by relaying a full
     * reset (source = ALL maps to a local reset on each peripheral). A relay
     * failure must not fail the whole factory reset - the local reset already
     * succeeded, and a peripheral may simply be disconnected. */
    cormoran_zmk_custom_settings_Request relay_req = cormoran_zmk_custom_settings_Request_init_zero;
    relay_req.which_request_type = cormoran_zmk_custom_settings_Request_reset_settings_tag;
    relay_req.request_type.reset_settings.has_scope = true;
    relay_req.request_type.reset_settings.scope.has_source = true;
    relay_req.request_type.reset_settings.scope.source = ZMK_CUSTOM_SETTING_SOURCE_ALL;
    int relay_ret = relay_request_to_peripherals(&relay_req);
    if (relay_ret < 0) {
        LOG_WRN("Failed to relay custom settings reset to peripherals: %d", relay_ret);
    }
#endif

    return 0;
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(custom_settings, custom_settings_reset);
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
#include "../../tests/support/custom_settings_handler_assertions_0.inc"
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST) &&                                                 \
    IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_CHUNKED_RPC) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
#include "../../tests/support/custom_settings_handler_assertions_1.inc"
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
#include "../../tests/support/custom_settings_handler_assertions_2.inc"
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
static int relay_request_listener(const zmk_event_t *eh) {
    const struct zmk_custom_settings_relay_request *ev = as_zmk_custom_settings_relay_request(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int ret = k_msgq_put(&relay_request_msgq, ev, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("Failed to queue relayed custom settings request: %d", ret);
        return ZMK_EV_EVENT_BUBBLE;
    }

    ret = k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &relay_request_work);
    if (ret < 0) {
        LOG_WRN("Failed to submit relayed custom settings request work: %d", ret);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static void relay_notification_work_handler(struct k_work *work);

K_MSGQ_DEFINE(relay_notification_msgq, sizeof(struct zmk_custom_settings_relay_notification),
              CONFIG_ZMK_CUSTOM_SETTINGS_RELAY_NOTIFICATION_QUEUE_SIZE, 4);
K_WORK_DEFINE(relay_notification_work, relay_notification_work_handler);

static void relay_notification_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct zmk_custom_settings_relay_notification ev;
    while (k_msgq_get(&relay_notification_msgq, &ev, K_NO_WAIT) == 0) {
        k_mutex_lock(&notification_buffer_lock, K_FOREVER);
        notification_buffer = (cormoran_zmk_custom_settings_Notification)
            cormoran_zmk_custom_settings_Notification_init_zero;
        int ret = relayed_notification_to_public(&ev, &notification_buffer);
        if (ret < 0) {
            LOG_WRN("Failed to convert relayed custom settings notification: %d", ret);
            k_mutex_unlock(&notification_buffer_lock);
            continue;
        }

        ret = raise_encoded_studio_notification(&notification_buffer);
        if (ret < 0) {
            LOG_WRN("Failed to raise relayed custom settings notification: %d", ret);
        }
        k_mutex_unlock(&notification_buffer_lock);
    }
}

static int relay_notification_listener(const zmk_event_t *eh) {
    const struct zmk_custom_settings_relay_notification *ev =
        as_zmk_custom_settings_relay_notification(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int ret = k_msgq_put(&relay_notification_msgq, ev, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("Failed to queue relayed custom settings notification: %d", ret);
        return ZMK_EV_EVENT_BUBBLE;
    }

    ret = k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &relay_notification_work);
    if (ret < 0) {
        LOG_WRN("Failed to submit relayed custom settings notification work: %d", ret);
    }

    return ZMK_EV_EVENT_BUBBLE;
}
#endif

ZMK_LISTENER(custom_settings_relay_request, relay_request_listener);
ZMK_SUBSCRIPTION(custom_settings_relay_request, zmk_custom_settings_relay_request);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
ZMK_LISTENER(custom_settings_relay_notification, relay_notification_listener);
ZMK_SUBSCRIPTION(custom_settings_relay_notification, zmk_custom_settings_relay_notification);
#endif
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST) &&                                                 \
    IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include "../../tests/support/custom_settings_handler_assertions_3.inc"
#endif
