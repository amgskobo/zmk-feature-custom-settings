/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/sys/printk.h>
/* SYS_INIT can return an error without failing the native process. The owner
 * verifier requires every linked wrapper's completion and rejects nonzero rc. */
#define OWNER_CHECKED_SYS_INIT(fn, level, priority)                                                \
    static int owner_checked_##fn(void) {                                                          \
        int result = fn();                                                                         \
        printk("OWNER_TEST_RESULT " #fn "=%d\n", result);                                          \
        return result;                                                                             \
    }                                                                                              \
    SYS_INIT(owner_checked_##fn, level, priority)
