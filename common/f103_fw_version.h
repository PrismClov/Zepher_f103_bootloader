/*
 * Firmware version block embedded in the application image.
 *
 * The block lives in .rodata (never stripped due to "used"), so both the
 * running application and the bootloader (which scans slot A) can report
 * the installed firmware version.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef F103_FW_VERSION_H_
#define F103_FW_VERSION_H_

#include <stdint.h>

#define F103_FW_VERSION_MAGIC    0x31525646U /* "FVR1" */
#define F103_FW_VERSION_RESERVED 0x5A5AA5A5U

struct f103_fw_version {
	uint32_t magic;
	uint16_t major;
	uint16_t minor;
	uint16_t patch;
	uint16_t build;
	uint32_t reserved;
};

#define F103_FW_VERSION_WORD(major, minor, patch) \
	((((uint32_t)(major)) << 16) | (((uint32_t)(minor)) << 8) | \
	 ((uint32_t)(patch)))

#endif /* F103_FW_VERSION_H_ */
