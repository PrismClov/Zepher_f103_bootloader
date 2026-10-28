/*
 * Minimal Zephyr application linked for the F103 USB bootloader.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "f103_bootloader_api.h"
#include "runtime_usb.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("F103 application started");

	if (runtime_usb_init() < 0) {
		LOG_ERR("Runtime USB upgrade interface failed");
	} else if (f103_bootloader_confirm_image()) {
		/* Replace with the real product self-test before confirming. */
		LOG_INF("Self-test passed, image confirmed (image_ok)");
	} else {
		LOG_INF("No pending image to confirm");
	}

	while (true) {
		if (runtime_usb_wait_for_boot_timeout(K_SECONDS(5)) == 0) {
			LOG_INF("Host requested bootloader");
			runtime_usb_restart_to_bootloader();
		}
		LOG_INF("Application alive");
	}
}

