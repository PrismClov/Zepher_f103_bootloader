/*
 * Shared layout and USB HID IAP protocol for the F103 bootloader.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef F103_IAP_H_
#define F103_IAP_H_

#include <stdint.h>

#define F103_FLASH_BASE         0x08000000U
#define F103_BOOT_SIZE          (32U * 1024U)
#define F103_SLOT_A_BASE        0x08008000U
#define F103_SLOT_B_BASE        0x08013C00U
#define F103_SLOT_SIZE          (47U * 1024U)
#define F103_FLASH_END          0x08020000U
#define F103_FLASH_PAGE_SIZE    1024U

#define F103_BOOT_OFFSET        0x00000000U
#define F103_SLOT_A_OFFSET      0x00008000U
#define F103_SLOT_B_OFFSET      0x00013C00U
#define F103_META_A_OFFSET      0x0001F800U
#define F103_META_B_OFFSET      0x0001FC00U
#define F103_META_PAGE_SIZE     (1U * 1024U)

/*
 * v2 metadata ("2BMT"): adds a pending TEST state with a persistent boot
 * attempt counter and a known-good slot for automatic revert. Records in
 * the v1 format ("ABMT") are treated as invalid.
 */
#define F103_META_MAGIC         0x544D4232U /* "2BMT" */
#define F103_SLOT_VALID         0x56414C44U /* "DALV" */
#define F103_BOOT_REQUEST_MAGIC 0xB007F103U
#define F103_IAP_VERSION        0x00020000U

/* Metadata flags: the active slot is a test image awaiting image_ok. */
#define F103_META_FLAG_PENDING  0x00000001U

/* Test boots allowed before the bootloader reverts to the known-good slot. */
#define F103_TEST_BOOT_MAX_ATTEMPTS 3U

#define F103_HID_REPORT_ID_IN   0x01U
#define F103_HID_REPORT_ID_OUT  0x02U
#define F103_HID_REPORT_SIZE    64U
#define F103_IAP_HEADER_SIZE    9U
#define F103_HID_DATA_MAX       (F103_HID_REPORT_SIZE - 1U - F103_IAP_HEADER_SIZE)

#define F103_IAP_MAGIC0         0xA5U
#define F103_IAP_MAGIC1         0x5AU

enum f103_iap_cmd {
	F103_IAP_PING = 1,
	F103_IAP_BEGIN = 2,
	F103_IAP_DATA = 3,
	F103_IAP_END = 4,
	F103_IAP_RUN = 5,
	F103_IAP_ENTER_BOOTLOADER = 6,
};

enum f103_iap_role {
	F103_IAP_ROLE_BOOTLOADER = 1,
	F103_IAP_ROLE_APPLICATION = 2,
};

enum f103_iap_status {
	F103_IAP_OK = 0,
	F103_IAP_ERR_ARG = 1,
	F103_IAP_ERR_STATE = 2,
	F103_IAP_ERR_RANGE = 3,
	F103_IAP_ERR_FLASH = 4,
	F103_IAP_ERR_CRC = 5,
	F103_IAP_ERR_IMAGE = 6,
	F103_IAP_ERR_COMMAND = 7,
};

struct f103_slot_meta {
	uint32_t start;
	uint32_t size;
	uint32_t crc32;
	uint32_t state;
};

struct f103_meta {
	uint32_t magic;
	uint32_t sequence;
	uint32_t active_slot;
	uint32_t known_good_slot;
	uint32_t boot_attempts;
	uint32_t flags;
	struct f103_slot_meta slot[2];
	uint32_t record_crc;
};

#endif /* F103_IAP_H_ */
