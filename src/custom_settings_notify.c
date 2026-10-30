/* SPDX-License-Identifier: MIT */
/* Retain borrowed array views across synchronous out-of-lock notifications. */
#include "custom_settings_internal.h"

void retain_setting_notification_locked(const struct zmk_custom_setting *setting) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
    array_view_retain_locked(setting);
#else
    ARG_UNUSED(setting);
#endif
}
void finish_setting_change_locked(const struct zmk_custom_setting *setting,
                                  enum zmk_custom_setting_changed_kind kind,
                                  bool suppress_notification, bool notify) {
    custom_settings_state_unlock();
    if (notify) {
        raise_setting_changed_with_notification(setting, kind, suppress_notification);
    }
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
    custom_settings_memory_lock();
    array_view_release_locked(setting);
    custom_settings_memory_unlock();
#endif
}
