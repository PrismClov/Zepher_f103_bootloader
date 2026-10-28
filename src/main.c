/*
 * USB HID IAP bootloader for STM32F103CBT6.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "f103_bootloader_api.h"
#include "f103_hid_descriptor.h"
#include "f103_iap.h"
#include "flash_update.h"

#include <string.h>

#include <stm32f1xx.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

#define BOOT_WINDOW_MS       3000
#define CONFIG_WINDOW_MS     2500
#define IAP_THREAD_STACK     2048
#define IAP_THREAD_PRIORITY  4

struct hid_message {
	uint8_t data[F103_HID_REPORT_SIZE];
	uint32_t length;
};

K_MSGQ_DEFINE(hid_rx_queue, sizeof(struct hid_message), 2, 4);
K_SEM_DEFINE(hid_tx_done, 0, 1);

static const struct device *hid_device;
static volatile bool upgrade_session;
static volatile bool run_requested;
static volatile bool image_committed;
static int64_t boot_deadline;

/*
 * Most STM32F103 boards have a permanently connected 1.5 kOhm pull-up on
 * USB D+. Pull D+ low while the application runs so the host sees a real
 * disconnect. Reset restores PA12 to its input state and reattaches USB.
 */
static void usb_disconnect_dp(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
	(void)RCC->APB2ENR;

	/* PA12: output push-pull, 2 MHz, then drive low. */
	GPIOA->CRH = (GPIOA->CRH & ~(0xFU << 16)) | (0x2U << 16);
	GPIOA->BSRR = GPIO_BSRR_BR12;
	__DSB();
}

static void jump_to_application(uint32_t base)
{
	__disable_irq();

	SysTick->CTRL = 0U;
	SysTick->LOAD = 0U;
	SysTick->VAL = 0U;

	for (uint32_t i = 0U; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFU;
		NVIC->ICPR[i] = 0xFFFFFFFFU;
	}

	SCB->VTOR = base;
	__DSB();
	__ISB();

	/*
	 * The register write, MSP switch and branch are deliberately one asm
	 * sequence. After switching MSP, this function must not touch its own
	 * bootloader stack again.
	 */
	__asm volatile(
		"ldr r1, [%[base]]\n\t"
		"ldr r2, [%[base], #4]\n\t"
		"movs r3, #0\n\t"
		"msr control, r3\n\t"
		"isb\n\t"
		"msr msp, r1\n\t"
		"msr basepri, r3\n\t"
		"cpsie f\n\t"
		"cpsie i\n\t"
		"isb\n\t"
		"bx r2\n\t"
		:
		: [base] "r" (base)
		: "r1", "r2", "r3", "memory");

	__builtin_unreachable();
}

static int send_report(const uint8_t *report, size_t length)
{
	uint32_t written = 0U;

	k_sem_reset(&hid_tx_done);

	const int err = hid_int_ep_write(hid_device, report, length, &written);
	if (err < 0) {
		return err;
	}

	if (k_sem_take(&hid_tx_done, K_MSEC(1000)) != 0) {
		return -ETIMEDOUT;
	}

	return written == length ? 0 : -EIO;
}

static int send_response(uint8_t command, uint8_t sequence, uint8_t status,
			 uint32_t value, const uint8_t *extra, size_t extra_len)
{
	uint8_t report[F103_HID_REPORT_SIZE] = {0U};

	report[0] = F103_HID_REPORT_ID_IN;
	report[1] = status;
	report[2] = command;
	report[3] = sequence;
	sys_put_le32(value, &report[4]);
	if (extra != NULL) {
		extra_len = MIN(extra_len, sizeof(report) - 8U);
		memcpy(&report[8], extra, extra_len);
	}

	return send_report(report, sizeof(report));
}

static void process_request(const uint8_t *request, size_t length)
{
	uint8_t sequence;
	uint8_t command;
	uint8_t data_length;
	uint32_t address;
	const uint8_t *data;
	uint8_t status = F103_IAP_OK;
	uint32_t value = 0U;
	uint8_t extra[20] = {0U};
	bool run_after_response = false;
	int err = 0;

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
	address = sys_get_le32(&request[4]);
	data_length = request[8];
	data = &request[F103_IAP_HEADER_SIZE];

	if (data_length > F103_HID_DATA_MAX ||
	    (size_t)data_length > length - F103_IAP_HEADER_SIZE) {
		status = F103_IAP_ERR_ARG;
		goto respond;
	}

	upgrade_session = true;

	switch (command) {
	case F103_IAP_PING:
		{
			const int active_slot = flash_update_active_slot();
			const int target_slot = flash_update_target_slot();

			value = F103_IAP_VERSION;
			sys_put_le32(flash_update_slot_base(active_slot), &extra[0]);
			sys_put_le32(flash_update_slot_base(target_slot), &extra[4]);
			sys_put_le32(flash_update_slot_size(), &extra[8]);
			extra[12] = F103_IAP_ROLE_BOOTLOADER;
			extra[13] = (uint8_t)MAX(active_slot, 0);
			extra[14] = (uint8_t)MAX(target_slot, 0);
			extra[15] = active_slot >= 0 ? 1U : 0U;

			{
				uint8_t pending = 0U;
				uint8_t attempts = 0U;
				uint8_t known_good = 0U;

				(void)flash_update_test_state(&pending, &attempts,
							      &known_good);
				extra[16] = pending;
				extra[17] = attempts;
				extra[18] = F103_TEST_BOOT_MAX_ATTEMPTS;
				extra[19] = known_good;
			}
		}
		break;

	case F103_IAP_BEGIN:
		err = flash_update_begin(address);
		if (err == -EINVAL) {
			status = F103_IAP_ERR_ARG;
		} else if (err < 0) {
			status = F103_IAP_ERR_FLASH;
		}
		break;

	case F103_IAP_DATA:
		if (address > F103_SLOT_SIZE ||
		    data_length > F103_SLOT_SIZE - address) {
			status = F103_IAP_ERR_RANGE;
			break;
		}

		err = flash_update_write(address, data, data_length);
		if (err == -ERANGE || err == -EINVAL) {
			status = F103_IAP_ERR_RANGE;
		} else if (err < 0) {
			status = F103_IAP_ERR_FLASH;
		}
		break;

	case F103_IAP_END: {
		uint32_t actual_crc = 0U;

		err = flash_update_finish(address, &actual_crc);
		image_committed = (err == 0);
		value = actual_crc;
		if (err == -EILSEQ) {
			status = F103_IAP_ERR_CRC;
		} else if (err == -EIO) {
			status = F103_IAP_ERR_FLASH;
		} else if (err < 0) {
			status = F103_IAP_ERR_IMAGE;
		}
		break;
	}

	case F103_IAP_RUN:
		if (!flash_update_image_valid(true)) {
			status = F103_IAP_ERR_IMAGE;
		} else {
			run_after_response = true;
		}
		break;

	default:
		status = F103_IAP_ERR_COMMAND;
		break;
	}

respond:
	err = send_response(command, sequence, status, value, extra, sizeof(extra));
	if (err == 0 && run_after_response) {
		run_requested = true;
	}
}

static void iap_thread(void *p1, void *p2, void *p3)
{
	struct hid_message message;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		if (k_msgq_get(&hid_rx_queue, &message, K_FOREVER) == 0) {
			process_request(message.data, message.length);
		}
	}
}

K_THREAD_DEFINE(iap_thread_id, IAP_THREAD_STACK, iap_thread, NULL, NULL, NULL,
		IAP_THREAD_PRIORITY, 0, 0);

static void hid_int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	k_sem_give(&hid_tx_done);
}

static void hid_int_out_ready_cb(const struct device *dev)
{
	struct hid_message message;
	uint32_t received = 0U;
	int err;

	err = hid_int_ep_read(dev, message.data, sizeof(message.data), &received);
	if (err < 0 || received == 0U) {
		return;
	}

	message.length = received;
	(void)k_msgq_put(&hid_rx_queue, &message, K_NO_WAIT);
}

static const struct hid_ops hid_ops = {
	.int_in_ready = hid_int_in_ready_cb,
	.int_out_ready = hid_int_out_ready_cb,
};

static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	ARG_UNUSED(param);

	if (status == USB_DC_CONFIGURED) {
		const int64_t now = k_uptime_get();

		if (boot_deadline < now + CONFIG_WINDOW_MS) {
			boot_deadline = now + CONFIG_WINDOW_MS;
		}
	}
}

static int init_usb(void)
{
	hid_device = device_get_binding("HID_0");
	if (hid_device == NULL) {
		return -ENODEV;
	}

	usb_hid_register_device(hid_device, f103_hid_report_descriptor,
				sizeof(f103_hid_report_descriptor), &hid_ops);
	usb_hid_init(hid_device);

	return usb_enable(usb_status_cb);
}

int main(void)
{
	const bool force_bootloader = f103_bootloader_consume_request();
	uint32_t boot_base = 0U;
	const bool image_valid = flash_update_get_boot_info(&boot_base, true);
	int err;

	boot_deadline = k_uptime_get() + BOOT_WINDOW_MS;

	err = init_usb();
	if (err < 0) {
		if (image_valid && !force_bootloader) {
			usb_disconnect_dp();
			jump_to_application(boot_base);
		}

		while (true) {
			k_sleep(K_FOREVER);
		}
	}

	while (true) {
		if (run_requested) {
			/*
			 * A pending record downloaded during this session counts
			 * its first TEST boot here. Otherwise the boot decision
			 * was already made once at startup.
			 */
			const bool can_boot = image_committed
				? flash_update_get_boot_info(&boot_base, true)
				: (boot_base != 0U &&
				   flash_update_image_valid(true));

			if (can_boot) {
				(void)usb_disable();
				k_sleep(K_MSEC(100));
				usb_disconnect_dp();
				jump_to_application(boot_base);
			}

			run_requested = false;
		}

		if (!force_bootloader && image_valid && !upgrade_session &&
		    !run_requested && k_uptime_get() >= boot_deadline) {
			(void)usb_disable();
			k_sleep(K_MSEC(50));
			usb_disconnect_dp();
			jump_to_application(boot_base);
		}

		k_sleep(K_MSEC(20));
	}
}

