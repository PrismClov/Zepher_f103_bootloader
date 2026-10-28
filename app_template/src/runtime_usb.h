/*
 * Runtime USB upgrade interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RUNTIME_USB_H_
#define RUNTIME_USB_H_

#include <zephyr/kernel.h>

int runtime_usb_init(void);

/* Wait until the host requests a transition into the bootloader. */
int runtime_usb_wait_for_boot_timeout(k_timeout_t timeout);

/* Disconnect D+, persist the request and reset into the bootloader. */
void runtime_usb_restart_to_bootloader(void) __attribute__((noreturn));

#endif /* RUNTIME_USB_H_ */

