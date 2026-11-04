/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Array settings (CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY).
 *
 * A registered ZMK_CUSTOM_SETTING_ARRAY_DEFINE array has exactly one
 * compile-time struct zmk_custom_setting descriptor (array_index ==
 * ZMK_CUSTOM_SETTING_ARRAY_NONE); its elements are not individually
 * registered. Instead, zmk_custom_setting_find_array_element hands out a
 * short-lived "index view" - a struct zmk_custom_setting pulled from the
 * array_view_pool below and pointed at the array's shared array_state - so
 * callers still get an ordinary `const struct zmk_custom_setting *` for one
 * element without an O(max array length) registration cost. Every element's
 * value/dirty/has_persistent bit lives in the array's own array_state
 * buffers (see struct zmk_custom_setting_array_state in the public header),
 * not in a per-element struct zmk_custom_setting_state.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <cormoran/zmk/custom_settings.h>

#include "custom_settings_internal.h"

/*
 * Array element "index views": zmk_custom_setting_find_array_element hands
 * out a struct zmk_custom_setting * for one (array descriptor, index) pair
 * without registering a real STRUCT_SECTION_ITERABLE entry per element (see
 * struct zmk_custom_setting_array_state in the header). Views are pulled
 * from this small direct-mapped pool and reused for the same (array, index)
 * pair across calls, so a pointer returned earlier keeps working correctly
 * (including any temp_slot it owns) even if other lookups happened in
 * between - this matters for callers like test_temporary_override_pool that
 * hold onto array element pointers across several calls.
 *
 * Pool size is bounded by CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE;
 * it only needs to cover how many distinct array elements are referenced at
 * once (RPC handling, list enumeration, tests), not array length itself.
 */
struct zmk_custom_setting_array_view_slot {
    bool in_use;
    uint32_t notification_refs;
    /* A view is a runtime-built RAM instance of the (normally const/flash)
     * struct zmk_custom_setting, so it embeds its own state block; the only
     * view state that actually matters is the TEMPORARY_ACTIVE flag +
     * temp_slot (element values/dirty/has_persistent live in the shared
     * array_state). */
    struct zmk_custom_setting_state state;
    struct zmk_custom_setting view;
};

static struct zmk_custom_setting_array_view_slot
    array_view_pool[CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE];

/* Non-static: zmk_custom_setting_discard/zmk_custom_setting_reset's array
 * branches (custom_settings.c) call this directly for whole-array discard/
 * reset. Declared in custom_settings_internal.h. */
struct zmk_custom_setting *array_view_acquire(const struct zmk_custom_setting *array_descriptor,
                                              uint32_t index) {
    struct zmk_custom_setting_array_view_slot *free_slot = NULL;
    struct zmk_custom_setting_array_view_slot *recyclable = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        struct zmk_custom_setting_array_view_slot *slot = &array_view_pool[i];
        if (slot->in_use && slot->view.array_state == array_descriptor->array_state &&
            slot->view.array_index == index) {
            return &slot->view;
        }
        if (!recyclable && slot->in_use && slot->notification_refs == 0) {
            recyclable = slot;
        }
        if (!free_slot && !slot->in_use) {
            free_slot = slot;
        }
    }

    if (!free_slot) {
        /* Pool exhausted: recycle the first unretained slot. Before overwriting it, release any
         * temporary override the evicted view still owns. Ownership in
         * temp_slots is tracked by view pointer, so without this the evicted
         * element's temp slot would be leaked forever (in_use, but its owner
         * address is about to be reused for a different element) and could
         * later be aliased by owner-pointer to the new element that reuses
         * this same view address. A caller still holding a stale pointer for
         * the evicted (array, index) will observe the override cleared along
         * with the value being redirected to the new element - an inherent
         * limitation of holding a pointer past pool eviction. */
        if (!recyclable) {
            return NULL;
        }
        free_slot = recyclable;
        clear_temporary_locked(&free_slot->view);
    }

    free_slot->in_use = true;
    free_slot->view = *array_descriptor;
    free_slot->view.array_index = index;
    free_slot->view.array_state = array_descriptor->array_state;
    free_slot->view.default_value = NULL;
    /* Redirect the copied descriptor's state pointer away from the array
     * descriptor's own state block to this view's embedded one, then reset
     * it. */
    free_slot->state = (struct zmk_custom_setting_state){.temp_slot = -1};
    free_slot->view.state = &free_slot->state;
    return &free_slot->view;
}

/* A synchronous event must retain its borrowed view from the guarded
 * mutation until every listener returns. Do not hold a registry lock while
 * dispatching: callbacks may read/write settings or wait on settings work. */
void array_view_retain_locked(const struct zmk_custom_setting *setting) {
    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        if (setting == &array_view_pool[i].view) {
            __ASSERT(array_view_pool[i].notification_refs < UINT32_MAX, "view retain overflow");
            array_view_pool[i].notification_refs++;
            return;
        }
    }
}
void array_view_release_locked(const struct zmk_custom_setting *setting) {
    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        if (setting == &array_view_pool[i].view) {
            __ASSERT(array_view_pool[i].notification_refs > 0, "unbalanced view release");
            array_view_pool[i].notification_refs--;
            return;
        }
    }
}

/* Non-static: the core flag helpers (set_setting_dirty/setting_is_dirty/
 * setting_has_persistent_value/set_setting_has_persistent_value in
 * custom_settings.c) call these for an array element. Declared in
 * custom_settings_internal.h. */
bool *array_dirty_slot(const struct zmk_custom_setting *setting) {
    return &setting->array_state->dirty[setting->array_index];
}

bool *array_has_persistent_slot(const struct zmk_custom_setting *setting) {
    return &setting->array_state->has_persistent[setting->array_index];
}

const struct zmk_custom_setting *zmk_custom_setting_find_array(const char *custom_subsystem_id,
                                                               const char *key) {
    if (!key) {
        return NULL;
    }
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!zmk_custom_setting_is_array(setting)) {
            continue;
        }

        if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
            strncmp(setting->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
            continue;
        }

        if (strncmp(setting->array_key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0) {
            return setting;
        }
    }

    return NULL;
}

const struct zmk_custom_setting *
zmk_custom_setting_find_array_element(const char *custom_subsystem_id, const char *key,
                                      uint32_t index) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array(custom_subsystem_id, key);
    if (!array_setting || index >= array_setting->array_state->max_size) {
        return NULL;
    }

    custom_settings_memory_lock();
    struct zmk_custom_setting *view = array_view_acquire(array_setting, index);
    custom_settings_memory_unlock();

    return view;
}

static int validate_array_size(const struct zmk_custom_setting *setting, uint32_t array_size) {
    if (!zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    return array_size <= setting->array_state->max_size ? 0 : -ERANGE;
}

/* Clear any active temporary override on array elements at or past
 * new_size. Only live view-pool slots for this array can have an active
 * override (temp_slot lives on the view instance, not in array_state), so
 * this scans the bounded pool instead of the (potentially much larger)
 * array itself. */
static void
clear_temporary_past_size_locked(const struct zmk_custom_setting_array_state *array_state,
                                 uint32_t new_size) {
    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        struct zmk_custom_setting_array_view_slot *slot = &array_view_pool[i];
        if (slot->in_use && slot->view.array_state == array_state &&
            slot->view.array_index >= new_size) {
            clear_temporary_locked(&slot->view);
        }
    }
}

/* Non-static: zmk_custom_setting_discard/zmk_custom_setting_reset
 * (custom_settings.c) call these two setters directly for whole-array
 * discard/reset. Declared in custom_settings_internal.h. */
void set_array_memory_size_locked(const struct zmk_custom_setting *array_element,
                                  uint32_t array_size) {
    struct zmk_custom_setting_array_state *array_state = array_element->array_state;

    array_state->size = array_size;
    clear_temporary_past_size_locked(array_state, array_size);
}

void set_array_persistent_size_locked(const struct zmk_custom_setting *array_element,
                                      uint32_t array_size) {
    array_element->array_state->persistent_size = array_size;
}

uint32_t zmk_custom_setting_array_size(const struct zmk_custom_setting *setting) {
    if (!zmk_custom_setting_is_array(setting)) {
        return 0;
    }

    custom_settings_memory_lock();
    uint32_t array_size = setting->array_state->size;
    custom_settings_memory_unlock();

    return array_size;
}

uint32_t zmk_custom_setting_array_max_size(const struct zmk_custom_setting *setting) {
    return zmk_custom_setting_is_array(setting) ? setting->array_state->max_size : 0;
}

/* Non-static: zmk_custom_setting_reset (custom_settings.c) calls this
 * directly to erase the "_size" marker on discard/reset. Declared in
 * custom_settings_internal.h. */
int array_size_storage_name(const struct zmk_custom_setting *setting, char *name,
                            size_t name_size) {
    if (!zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    int ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s/%s", setting->custom_subsystem_id,
                       setting->array_key, ARRAY_SIZE_STORAGE_KEY);
    if (ret < 0 || ret >= name_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

/* Delete on-flash records for slots at or past array_size. Operates
 * directly on the descriptor's array_state buffer (O(max_size), bounded by
 * the array's own max element count). */
static int delete_inactive_array_values_locked(const struct zmk_custom_setting *array_descriptor,
                                               uint32_t array_size) {
    struct zmk_custom_setting_array_state *array_state = array_descriptor->array_state;

    for (uint32_t index = array_size; index < array_state->max_size; index++) {
        if (!array_state->has_persistent[index]) {
            continue;
        }

        struct zmk_custom_setting element = *array_descriptor;
        element.array_index = index;
        const struct zmk_custom_setting *view = &element;
        char name[SETTINGS_MAX_NAME_LEN];
        int ret = setting_storage_name(view, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        ret = settings_delete(name);
        if (ret != 0 && ret != -ENOENT) {
            return ret;
        }

        array_state->has_persistent[index] = false;
        array_state->dirty[index] = true;
    }

    return 0;
}

/* Save all active elements (indices [0, array_size)) plus the "_size"
 * marker, then delete any now-inactive elements. Operates directly on the
 * descriptor's array_state buffer, so cost is O(array_size) rather than a
 * walk of the full setting registry. Non-static: save_setting_locked
 * (custom_settings.c) calls this directly for an array setting. Declared in
 * custom_settings_internal.h. */
int save_array_locked(const struct zmk_custom_setting *array_descriptor) {
    char size_name[SETTINGS_MAX_NAME_LEN];
    int ret = array_size_storage_name(array_descriptor, size_name, sizeof(size_name));
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_array_state *array_state = array_descriptor->array_state;
    uint32_t array_size = array_state->size;
    ret = settings_save_one(size_name, &array_size, sizeof(array_size));
    if (ret < 0) {
        return ret;
    }

    for (uint32_t index = 0; index < array_size; index++) {
        struct zmk_custom_setting element = *array_descriptor;
        element.array_index = index;
        const struct zmk_custom_setting *view = &element;
        char name[SETTINGS_MAX_NAME_LEN];
        ret = setting_storage_name(view, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        const void *data;
        size_t len;
        ret = value_to_storage(&array_state->values[index], &data, &len);
        if (ret < 0) {
            return ret;
        }

        ret = settings_save_one(name, data, len);
        if (ret < 0) {
            return ret;
        }

        array_state->has_persistent[index] = true;
        array_state->dirty[index] = false;
    }

    set_array_persistent_size_locked(array_descriptor, array_size);
    return delete_inactive_array_values_locked(array_descriptor, array_size);
}

int zmk_custom_setting_read_array_by_key(const char *custom_subsystem_id, const char *key,
                                         uint32_t index, struct zmk_custom_setting_value *value) {
    custom_settings_memory_lock();
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array_element(custom_subsystem_id, key, index);
    int ret = setting ? zmk_custom_setting_read(setting, value) : -ENOENT;
    custom_settings_memory_unlock();
    return ret;
}

int zmk_custom_setting_write_array_by_key(const char *custom_subsystem_id, const char *key,
                                          uint32_t index,
                                          const struct zmk_custom_setting_value *value,
                                          enum zmk_custom_setting_write_mode mode) {
    custom_settings_state_lock();
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array_element(custom_subsystem_id, key, index);
    retain_setting_notification_locked(setting);
    int ret = setting ? zmk_custom_setting_validate(setting, value) : -ENOENT;
    if (ret == 0) {
        ret =
            index < setting->array_state->size ? write_value_locked(setting, value, mode) : -ENOENT;
    }
    finish_setting_change_locked(setting,
                                 mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                     ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                     : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                 false, ret == 0);
    return ret;
}

/* Structural changes have no temporary length/ordering backing store. */
static bool structural_write_mode(enum zmk_custom_setting_write_mode mode) {
    return mode == ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY ||
           mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST;
}

/* Caller holds the paired guard from selection through mutation. */
int write_array_element_locked(const struct zmk_custom_setting *setting,
                               const struct zmk_custom_setting_value *value, uint32_t array_size,
                               enum zmk_custom_setting_write_mode mode) {
    if (!zmk_custom_setting_is_array(setting) || setting->array_index >= array_size) {
        return -EINVAL;
    }
    if (!structural_write_mode(mode) && (mode != ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY ||
                                         array_size != setting->array_state->size)) {
        return -EINVAL;
    }
    int ret = validate_array_size(setting, array_size);
    if (ret == 0) {
        ret = zmk_custom_setting_validate(setting, value);
    }
    if (ret < 0) {
        return ret;
    }
    if (mode != ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) {
        set_array_memory_size_locked(setting, array_size);
    }
    return write_value_locked(setting, value, mode);
}

int zmk_custom_setting_write_array_element(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *value,
                                           uint32_t array_size,
                                           enum zmk_custom_setting_write_mode mode) {
    if (!setting || !value) {
        return -EINVAL;
    }
    custom_settings_state_lock();
    retain_setting_notification_locked(setting);
    int ret = write_array_element_locked(setting, value, array_size, mode);
    finish_setting_change_locked(setting,
                                 mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                     ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                     : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                 false, ret == 0);
    return ret;
}

int zmk_custom_setting_write_array_element_by_key(const char *custom_subsystem_id, const char *key,
                                                  uint32_t index,
                                                  const struct zmk_custom_setting_value *value,
                                                  uint32_t array_size,
                                                  enum zmk_custom_setting_write_mode mode) {
    if (!value) {
        return -EINVAL;
    }
    custom_settings_state_lock();
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array_element(custom_subsystem_id, key, index);
    retain_setting_notification_locked(setting);
    int ret = -ENOENT;
    if (setting) {
        if (array_size == 0) {
            array_size = MAX(setting->array_state->size, index + 1);
        }
        ret = write_array_element_locked(setting, value, array_size, mode);
    }
    finish_setting_change_locked(setting,
                                 mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                     ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                     : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                 false, ret == 0);
    return ret;
}

int zmk_custom_setting_array_push_back(const struct zmk_custom_setting *setting,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!setting || !value || !zmk_custom_setting_is_array(setting) ||
        !structural_write_mode(mode)) {
        return -EINVAL;
    }
    custom_settings_state_lock();
    uint32_t size = setting->array_state->size;
    int ret = -ERANGE;
    const struct zmk_custom_setting *tail = NULL;
    if (size < setting->array_state->max_size) {
        tail = array_view_acquire(setting, size);
        if (tail) {
            retain_setting_notification_locked(tail);
            ret = write_array_element_locked(tail, value, size + 1, mode);
        } else {
            ret = -ENOMEM;
        }
    }
    finish_setting_change_locked(tail,
                                 mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                     ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                     : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                 false, ret == 0);
    return ret;
}

int zmk_custom_setting_array_pop_back(const struct zmk_custom_setting *setting,
                                      struct zmk_custom_setting_value *value,
                                      enum zmk_custom_setting_write_mode mode) {
    if (!setting || !zmk_custom_setting_is_array(setting) || !structural_write_mode(mode)) {
        return -EINVAL;
    }
    custom_settings_state_lock();
    retain_setting_notification_locked(setting);
    uint32_t size = setting->array_state->size;
    if (size == 0) {
        finish_setting_change_locked(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED, false,
                                     false);
        return -ENOENT;
    }
    struct zmk_custom_setting *tail = array_view_acquire(setting, size - 1);
    if (!tail) {
        finish_setting_change_locked(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED, false,
                                     false);
        return -ENOMEM;
    }
    if (value) {
        copy_value(value, effective_value(tail));
    }
    set_array_memory_size_locked(setting, size - 1);
    clear_temporary_locked(tail);
    int ret = mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST ? save_setting_locked(tail) : 0;
    finish_setting_change_locked(setting,
                                 mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                     ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                     : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                 false, ret == 0);
    return ret;
}

int zmk_custom_setting_array_insert_at(const struct zmk_custom_setting *const_setting,
                                       uint32_t index, const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !value || !zmk_custom_setting_is_array(const_setting) ||
        !structural_write_mode(mode)) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;
    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    custom_settings_state_lock();
    retain_setting_notification_locked(setting);
    struct zmk_custom_setting_array_state *array_state = setting->array_state;
    uint32_t array_size = array_state->size;

    if (index > array_size) {
        finish_setting_change_locked(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED, false,
                                     false);
        return -ERANGE;
    }
    if (array_size >= array_state->max_size) {
        finish_setting_change_locked(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED, false,
                                     false);
        return -ERANGE;
    }

    /* Shift [index, array_size) one slot later over the single contiguous
     * buffer instead of requiring N individual element writes - the whole
     * point of a contiguous backing buffer. Any temp override on a shifted
     * element is cleared (its underlying value moved), matching how
     * set_array_memory_size_locked/clear_temporary_past_size_locked already
     * treat elements that fall out of the valid range on resize. */
    uint32_t move_count = array_size - index;
    if (move_count > 0) {
        memmove(&array_state->values[index + 1], &array_state->values[index],
                move_count * sizeof(array_state->values[0]));
        clear_temporary_past_size_locked(array_state, index);
    }

    copy_value(&array_state->values[index], value);
    /* Dirty follows changed values; persistence belongs to storage indices. */
    for (uint32_t i = index; i <= array_size; i++) {
        array_state->dirty[i] = true;
    }
    array_state->size = array_size + 1;

    ret = 0;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = save_setting_locked(setting);
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        /* Insertion always establishes a real memory value; "temporary
         * insert" is not a meaningful combination. */
        ret = -EINVAL;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    finish_setting_change_locked(setting,
                                 mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                     ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                     : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                 false, ret == 0);
    return ret;
}

int zmk_custom_setting_array_remove_at(const struct zmk_custom_setting *const_setting,
                                       uint32_t index, struct zmk_custom_setting_value *out_value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !zmk_custom_setting_is_array(const_setting) ||
        !structural_write_mode(mode)) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    custom_settings_state_lock();
    retain_setting_notification_locked(setting);
    struct zmk_custom_setting_array_state *array_state = setting->array_state;
    uint32_t array_size = array_state->size;

    if (index >= array_size) {
        finish_setting_change_locked(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED, false,
                                     false);
        return -ENOENT;
    }

    if (out_value) {
        struct zmk_custom_setting *removed_view = array_view_acquire(setting, index);
        if (!removed_view) {
            finish_setting_change_locked(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED, false,
                                         false);
            return -ENOMEM;
        }
        copy_value(out_value, effective_value(removed_view));
    }
    clear_temporary_past_size_locked(array_state, index);

    /* Shift [index + 1, array_size) one slot earlier over the single
     * contiguous buffer instead of requiring N individual element writes. */
    uint32_t move_count = array_size - index - 1;
    if (move_count > 0) {
        memmove(&array_state->values[index], &array_state->values[index + 1],
                move_count * sizeof(array_state->values[0]));
        clear_temporary_past_size_locked(array_state, array_size - 1);
    }

    array_state->size = array_size - 1;
    /* Stored records remain at their original indices until save succeeds. */
    for (uint32_t i = index; i < array_size; i++) {
        array_state->dirty[i] = true;
    }

    int ret = 0;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = save_setting_locked(setting);
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        break;
    default:
        ret = -EINVAL;
        break;
    }
    finish_setting_change_locked(setting,
                                 mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                     ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                     : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                                 false, ret == 0);
    return ret;
}

/* Discard one array element view's in-memory value back to persisted/
 * default, and (once per call, harmless if repeated) restore the whole
 * array's active length to its persisted length. Called once per active
 * element by apply_scope so zmk_custom_settings_discard_scope's
 * affected_count matches "one per active element". Non-static:
 * zmk_custom_setting_discard
 * (custom_settings.c) calls this directly per active element. Declared in
 * custom_settings_internal.h. */
int discard_array_element_locked(struct zmk_custom_setting *view) {
    struct zmk_custom_setting_array_state *array_state = view->array_state;
    uint32_t index = view->array_index;

    if (array_state->has_persistent[index]) {
        /* No RAM-resident persistent_value copy is kept; re-read the
         * persisted value from flash straight into the buffer slot.
         * Discard is a rare, explicit user action, so a flash read here is
         * fine. */
        int ret = reload_setting_locked(view);
        if (ret < 0) {
            return ret;
        }
    } else {
        copy_value(&array_state->values[index], &array_state->defaults[index]);
    }
    array_state->dirty[index] = false;
    clear_temporary_locked(view);
    return 0;
}

/* Reset one array element view's persisted/in-memory value to its
 * compile-time default. Used both directly (whole-array reset) and once per
 * active element by apply_scope. Non-static: zmk_custom_setting_reset
 * (custom_settings.c) calls this directly per active element. Declared in
 * custom_settings_internal.h. */
int reset_array_element_locked(struct zmk_custom_setting *view) {
    struct zmk_custom_setting_array_state *array_state = view->array_state;
    uint32_t index = view->array_index;

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(view, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    ret = settings_delete(name);
    if (ret == -ENOENT) {
        ret = 0;
    }
    if (ret < 0) {
        return ret;
    }

    copy_value(&array_state->values[index], &array_state->defaults[index]);
    array_state->has_persistent[index] = false;
    array_state->dirty[index] = false;
    clear_temporary_locked(view);
    return 0;
}

/* Non-static: custom_settings_handle_set (custom_settings.c) calls this
 * directly to apply a loaded "_size" record. Declared in
 * custom_settings_internal.h. */
int array_size_from_storage(const struct zmk_custom_setting *array_element, const void *data,
                            size_t len) {
    uint32_t array_size;
    if (len != sizeof(array_size)) {
        return -EINVAL;
    }

    memcpy(&array_size, data, sizeof(array_size));
    int ret = validate_array_size(array_element, array_size);
    if (ret < 0) {
        return ret;
    }

    set_array_persistent_size_locked(array_element, array_size);
    set_array_memory_size_locked(array_element, array_size);
    return 0;
}

/* Non-static: custom_settings_handle_set (custom_settings.c) calls this to
 * recognize a loaded "_size" record. Declared in custom_settings_internal.h. */
bool split_array_size_key(const char *name, char *array_key, size_t array_key_size) {
    size_t name_len = bounded_strlen(name, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN);
    size_t suffix_len = sizeof("/" ARRAY_SIZE_STORAGE_KEY) - 1;

    if (name_len <= suffix_len ||
        strcmp(&name[name_len - suffix_len], "/" ARRAY_SIZE_STORAGE_KEY) != 0) {
        return false;
    }

    size_t key_len = name_len - suffix_len;
    if (key_len == 0 || key_len >= array_key_size) {
        return false;
    }

    memcpy(array_key, name, key_len);
    array_key[key_len] = '\0';
    return true;
}

/* Split a stored per-element name (e.g. "array_value/2") into its array_key
 * ("array_value") and numeric index (2). Array elements are not individually
 * registered (see ZMK_CUSTOM_SETTING_ARRAY_DEFINE), so
 * custom_settings_handle_set cannot resolve them with a plain
 * zmk_custom_setting_find(name) lookup and needs this to route into
 * zmk_custom_setting_find_array_element instead. Non-static:
 * custom_settings_handle_set (custom_settings.c) calls this to recognize a
 * loaded per-element record. Declared in custom_settings_internal.h. */
bool split_array_element_key(const char *name, char *array_key, size_t array_key_size,
                             uint32_t *index) {
    size_t name_len = bounded_strlen(name, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN);
    if (name[name_len] != '\0') {
        return false;
    }

    const char *slash = NULL;
    for (size_t i = name_len; i > 0; i--) {
        if (name[i - 1] == '/') {
            slash = &name[i - 1];
            break;
        }
    }
    if (!slash || slash == name) {
        return false;
    }

    const char *index_str = slash + 1;
    size_t index_str_len = &name[name_len] - index_str;
    if (index_str_len == 0) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < index_str_len; i++) {
        if (index_str[i] < '0' || index_str[i] > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(index_str[i] - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }

    size_t key_len = (size_t)(slash - name);
    if (key_len == 0 || key_len >= array_key_size) {
        return false;
    }

    memcpy(array_key, name, key_len);
    array_key[key_len] = '\0';
    *index = value;
    return true;
}
