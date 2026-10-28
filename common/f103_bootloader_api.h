/*
 * Runtime API used by an application to request the USB bootloader.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef F103_BOOTLOADER_API_H_
#define F103_BOOTLOADER_API_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Store the boot request in the STM32 backup domain and reset.
 * The call does not return on success.
 */
void f103_bootloader_request(void) __attribute__((noreturn));

/*
 * Store the boot request without resetting. The caller is responsible for
 * disconnecting USB and calling NVIC_SystemReset().
 */
void f103_bootloader_mark_request(void);

/*
 * Bootloader-only helper: consume and clear a pending request.
 */
bool f103_bootloader_consume_request(void);

/*
 * Called by the application after its self-test passed: writes image_ok so
 * the bootloader keeps the pending TEST image permanently. Returns false
 * when there is no valid metadata record to confirm.
 */
bool f103_bootloader_confirm_image(void);

#ifdef __cplusplus
}
#endif

#endif /* F103_BOOTLOADER_API_H_ */

