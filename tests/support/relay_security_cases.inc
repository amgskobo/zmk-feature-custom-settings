/* SPDX-License-Identifier: MIT */
/* Real relay codec, event dispatch, Studio lock API and deferred handlers. */
static unsigned int relay_security_notifications;
static unsigned int relay_security_requests;

static int relay_security_observer(const zmk_event_t *eh) {
    if (as_zmk_studio_custom_notification(eh)) {
        relay_security_notifications++;
    } else if (as_zmk_custom_settings_relay_request(eh)) {
        relay_security_requests++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_settings_relay_security_observer, relay_security_observer);
ZMK_SUBSCRIPTION(custom_settings_relay_security_observer, zmk_studio_custom_notification);
ZMK_SUBSCRIPTION(custom_settings_relay_security_observer, zmk_custom_settings_relay_request);

static int custom_settings_relay_security_test_init(void) {
    bool was_unlocked = studio_is_unlocked();
    cormoran_zmk_custom_settings_RelayNotification wire_notification =
        cormoran_zmk_custom_settings_RelayNotification_init_zero;
    wire_notification.has_notification = true;
    wire_notification.notification.which_notification_type =
        cormoran_zmk_custom_settings_Notification_setting_tag;
    wire_notification.notification.notification_type.setting.has_setting = true;
    cormoran_zmk_custom_settings_Setting *setting =
        &wire_notification.notification.notification_type.setting.setting;
    copy_string(setting->key, sizeof(setting->key), "relay_secure");
    setting->has_meta = true;
    setting->meta.read_permission =
        cormoran_zmk_custom_settings_SettingPermission_SETTING_PERMISSION_SECURE;
    setting->has_value = true;
    setting->value.which_value_type = cormoran_zmk_custom_settings_SettingValue_int32_value_tag;
    setting->value.value_type.int32_value = 42;
    struct zmk_custom_settings_relay_notification notification = {.source = 1};
    pb_ostream_t out = pb_ostream_from_buffer(notification.payload, sizeof(notification.payload));
    if (!pb_encode(&out, cormoran_zmk_custom_settings_RelayNotification_fields,
                   &wire_notification)) {
        return -EIO;
    }
    notification.payload_size = out.bytes_written;
    unsigned int before = relay_security_notifications;
    int ret = -EINVAL;

    zmk_studio_core_lock();
    raise_zmk_custom_settings_relay_notification(notification);
    relay_notification_work_handler(NULL);
    if (relay_security_notifications != before) {
        goto done;
    }

    zmk_studio_core_unlock();
    raise_zmk_custom_settings_relay_notification(notification);
    zmk_studio_core_lock();
    relay_notification_work_handler(NULL);
    if (relay_security_notifications != before) {
        goto done;
    }

    zmk_studio_core_unlock();
    raise_zmk_custom_settings_relay_notification(notification);
    zmk_studio_core_lock();
    zmk_studio_core_unlock();
    relay_notification_work_handler(NULL);
    if (relay_security_notifications != before) {
        goto done;
    }

    /* An unchanged unlocked session must still forward exactly one value. */
    raise_zmk_custom_settings_relay_notification(notification);
    relay_notification_work_handler(NULL);
    if (relay_security_notifications != before + 1) {
        goto done;
    }

    cormoran_zmk_custom_settings_Request request = cormoran_zmk_custom_settings_Request_init_zero;
    request.which_request_type = cormoran_zmk_custom_settings_Request_list_settings_tag;
    request.request_type.list_settings.has_scope = true;
    request.request_type.list_settings.scope.has_source = true;
    request.request_type.list_settings.scope.source = ZMK_CUSTOM_SETTING_SOURCE_ALL;
    before = relay_security_requests;
    if (schedule_list_settings_relay_to_peripherals(&request)) {
        goto done;
    }
    zmk_studio_core_lock();
    list_settings_relay_work_handler(NULL);
    if (relay_security_requests != before) {
        goto done;
    }
    zmk_studio_core_unlock();
    if (schedule_list_settings_relay_to_peripherals(&request)) {
        goto done;
    }
    zmk_studio_core_lock();
    zmk_studio_core_unlock();
    list_settings_relay_work_handler(NULL);
    if (relay_security_requests != before ||
        schedule_list_settings_relay_to_peripherals(&request)) {
        goto done;
    }
    list_settings_relay_work_handler(NULL);
    if (relay_security_requests != before + 1) {
        goto done;
    }
    ret = 0;
done:
    k_work_cancel_delayable(&list_settings_relay_work);
    if (was_unlocked) {
        zmk_studio_core_unlock();
    } else {
        zmk_studio_core_lock();
    }
    if (!ret) {
        LOG_INF("PASS: relay authorization and retirement controls");
    }
    return ret;
}

OWNER_CHECKED_SYS_INIT(custom_settings_relay_security_test_init, APPLICATION, 99);
