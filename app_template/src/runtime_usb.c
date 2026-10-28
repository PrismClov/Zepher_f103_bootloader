/*
 * Persistent USB HID service for entering the bootloader without a reset
 * button or a USB replug.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_usb.h"

#include "app_version.h"
#include "f103_bootloader_api.h"
#include "f103_fw_version.h"
#include "f103_hid_descriptor.h"
#include "f103_iap.h"

#include <stdbool.h>
#include <string.h>

#include <stm32f1xx.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

static const struct device *hid_device;

extern const struct f103_fw_version f103_app_fw_version;

#define APP_VERSION_WORD \
	F103_FW_VERSION_WORD(f103_app_fw_version.major, \
			     f103_app_fw_version.minor, \
			     f103_app_fw_version.patch)

static K_SEM_DEFINE(boot_request_sem, 0, 1);

static K_TIMER_DEFINE(boot_request_timer, NULL, NULL);

static void boot_timer_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_sem_give(&boot_request_sem);
}

static void send_response(uint8_t command, uint8_t sequence, uint8_t status,
			  uint32_t value, uint8_t role)
{
	uint8_t report[F103_HID_REPORT_SIZE] = {0U};

	report[0] = F103_HID_REPORT_ID_IN;
	report[1] = status;
	report[2] = command;
	report[3] = sequence;
	sys_put_le32(value, &report[4]);
	report[8 + 12] = role;
	sys_put_le32(APP_VERSION_WORD, &report[8 + 24]);
	report[8 + 28] = (uint8_t)f103_app_fw_version.build;
	report[8 + 29] = 1U; /* firmware version valid */

	(void)hid_int_ep_write(hid_device, report, sizeof(report), NULL);
}

static void process_request(const uint8_t *request, size_t length)
{
	uint8_t command;
	uint8_t sequence;
	uint8_t data_length;
	uint8_t status = F103_IAP_OK;
	uint32_t value = 0U;
	bool request_boot = false;

	if (length > 0U && request[0] == F103_HID_REPORT_ID_OUT) {
		request++;
		length--;
	}

	if (length < F103_IAP_HEADER_SIZE ||
	    request[0] != F103_IAP_MAGIC0 ||
	    request[1] != F103_IAP_MAGIC1) {
		return;
	}

	command = request[2];
	sequence = request[3];
	data_length = request[8];

	if (data_length > F103_HID_DATA_MAX ||
	    (size_t)data_length > length - F103_IAP_HEADER_SIZE) {
		status = F103_IAP_ERR_ARG;
	} else if (command == F103_IAP_PING) {
		value = F103_IAP_VERSION;
	} else if (command == F103_IAP_ENTER_BOOTLOADER) {
		request_boot = true;
	} else {
		status = F103_IAP_ERR_COMMAND;
	}

	send_response(command, sequence, status, value, F103_IAP_ROLE_APPLICATION);
	if (request_boot) {
		/*
		 * Give the IN transfer time to complete before main() powers
		 * down USB and resets the MCU.
		 */
		k_timer_start(&boot_request_timer, K_MSEC(50), K_NO_WAIT);
	}
}

static void hid_int_out_ready_cb(const struct device *dev)
{
	uint8_t report[F103_HID_REPORT_SIZE];
	uint32_t received = 0U;

	if (hid_int_ep_read(dev, report, sizeof(report), &received) == 0 &&
	    received > 0U) {
		process_request(report, received);
	}
}

static const struct hid_ops hid_ops = {
	.int_out_ready = hid_int_out_ready_cb,
};

static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	ARG_UNUSED(status);
	ARG_UNUSED(param);
}

static void usb_disconnect_dp(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
	(void)RCC->APB2ENR;

	GPIOA->CRH = (GPIOA->CRH & ~(0xFU << 16)) | (0x2U << 16);
	GPIOA->BSRR = GPIO_BSRR_BR12;
	__DSB();
}

int runtime_usb_init(void)
{
	hid_device = device_get_binding("HID_0");
	if (hid_device == NULL) {
		return -ENODEV;
	}

	usb_hid_register_device(hid_device, f103_hid_report_descriptor,
				sizeof(f103_hid_report_descriptor), &hid_ops);
	usb_hid_init(hid_device);

	k_timer_init(&boot_request_timer, boot_timer_expiry, NULL);
	return usb_enable(usb_status_cb);
}

int runtime_usb_wait_for_boot_timeout(k_timeout_t timeout)
{
	return k_sem_take(&boot_request_sem, timeout);
}

void runtime_usb_restart_to_bootloader(void)
{
	(void)usb_disable();
	k_sleep(K_MSEC(100));

	usb_disconnect_dp();
	f103_bootloader_mark_request();

	/* Leave D+ low long enough for the host to detect a disconnect. */
	k_sleep(K_MSEC(200));
	NVIC_SystemReset();

	__builtin_unreachable();
}

