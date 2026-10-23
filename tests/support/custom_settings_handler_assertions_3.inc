#include "init_result.h"
/* SPDX-License-Identifier: MIT */
static int relay_test_process_request(const cormoran_zmk_custom_settings_RelayRequest *src,
                                      cormoran_zmk_custom_settings_Response *resp) {
    struct zmk_custom_settings_relay_request request = {
        .source = 1,
    };
    pb_ostream_t stream = pb_ostream_from_buffer(request.payload, sizeof(request.payload));
    if (!pb_encode(&stream, cormoran_zmk_custom_settings_RelayRequest_fields, src)) {
        LOG_ERR("Split peripheral relay test encode failed: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    request.payload_size = stream.bytes_written;
    return process_relay_request(&request, resp);
}

static int relay_test_setting_ref(const char *key,
                                  cormoran_zmk_custom_settings_RelaySettingRef *ref) {
    ref->has_custom_subsystem_id = true;
    copy_string(ref->custom_subsystem_id, sizeof(ref->custom_subsystem_id), "test");
    ref->has_key = true;
    copy_string(ref->key, sizeof(ref->key), key);
    ref->has_source = true;
    ref->source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
    return 0;
}

static int custom_settings_split_peripheral_relay_test_init(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "int_value");
    if (!setting) {
        LOG_ERR("Split peripheral relay test setting not registered");
        return -ENOENT;
    }
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array("test", "array_value");
    const struct zmk_custom_setting *array_tail =
        zmk_custom_setting_find_array_element("test", "array_value", 2);
    if (!array_setting || !array_tail) {
        LOG_ERR("Split peripheral relay array setting not registered");
        return -ENOENT;
    }

    cormoran_zmk_custom_settings_RelayRequest request =
        cormoran_zmk_custom_settings_RelayRequest_init_zero;
    request.which_request_type = cormoran_zmk_custom_settings_RelayRequest_write_setting_tag;
    request.request_type.write_setting.has_setting = true;
    int ret = relay_test_setting_ref("int_value", &request.request_type.write_setting.setting);
    if (ret < 0) {
        return ret;
    }
    request.request_type.write_setting.has_value = true;
    request.request_type.write_setting.value.which_value_type =
        cormoran_zmk_custom_settings_SettingValue_int32_value_tag;
    request.request_type.write_setting.value.value_type.int32_value = 66;
    request.request_type.write_setting.mode =
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY;

    cormoran_zmk_custom_settings_Response resp = cormoran_zmk_custom_settings_Response_init_zero;
    ret = relay_test_process_request(&request, &resp);
    if (ret < 0) {
        LOG_ERR("Split peripheral relay request failed: %d", ret);
        return ret;
    }

    struct zmk_custom_setting_value value;
    ret = zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != 66) {
        LOG_ERR("Split peripheral relay test write failed");
        return -EINVAL;
    }

    printk("PASS: split_peripheral_relay\n");

    ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    cormoran_zmk_custom_settings_RelayRequest pop_request =
        cormoran_zmk_custom_settings_RelayRequest_init_zero;
    pop_request.which_request_type = cormoran_zmk_custom_settings_RelayRequest_pop_back_array_tag;
    pop_request.request_type.pop_back_array.has_setting = true;
    ret = relay_test_setting_ref("array_value", &pop_request.request_type.pop_back_array.setting);
    if (ret < 0) {
        return ret;
    }
    pop_request.request_type.pop_back_array.mode =
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY;

    resp = (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
    ret = relay_test_process_request(&pop_request, &resp);
    if (ret < 0) {
        LOG_ERR("Split peripheral relay pop_back failed: %d", ret);
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 2) {
        LOG_ERR("Split peripheral relay pop_back did not shrink array");
        return -EINVAL;
    }
    ret = zmk_custom_setting_read(array_tail, &value);
    if (ret != -ENOENT) {
        LOG_ERR("Expected split peripheral relay popped element read to fail, got %d", ret);
        return -EINVAL;
    }
    printk("PASS: split_peripheral_relay_pop_back size=2\n");

    cormoran_zmk_custom_settings_RelayRequest push_request =
        cormoran_zmk_custom_settings_RelayRequest_init_zero;
    push_request.which_request_type = cormoran_zmk_custom_settings_RelayRequest_push_back_array_tag;
    push_request.request_type.push_back_array.has_setting = true;
    ret = relay_test_setting_ref("array_value", &push_request.request_type.push_back_array.setting);
    if (ret < 0) {
        return ret;
    }
    push_request.request_type.push_back_array.has_value = true;
    push_request.request_type.push_back_array.value.which_value_type =
        cormoran_zmk_custom_settings_SettingScalarValue_int32_value_tag;
    push_request.request_type.push_back_array.value.value_type.int32_value = 44;
    push_request.request_type.push_back_array.mode =
        cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY;

    resp = (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
    ret = relay_test_process_request(&push_request, &resp);
    if (ret < 0) {
        LOG_ERR("Split peripheral relay push_back failed: %d", ret);
        return ret;
    }
    ret = zmk_custom_setting_read(array_tail, &value);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 3 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != 44) {
        LOG_ERR("Split peripheral relay push_back did not append array value");
        return -EINVAL;
    }
    printk("PASS: split_peripheral_relay_push_back array[2]=44 size=3\n");
    return 0;
}

OWNER_CHECKED_SYS_INIT(custom_settings_split_peripheral_relay_test_init, APPLICATION, 99);
