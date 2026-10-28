/*
 * Shared vendor HID report descriptor for the runtime and bootloader.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef F103_HID_DESCRIPTOR_H_
#define F103_HID_DESCRIPTOR_H_

#include "f103_iap.h"

static const uint8_t f103_hid_report_descriptor[] = {
	0x06, 0x00, 0xFF,       /* Usage Page (Vendor Defined 0xFF00) */
	0x09, 0x01,             /* Usage (1) */
	0xA1, 0x01,             /* Collection (Application) */
	0x85, F103_HID_REPORT_ID_IN,
	0x09, 0x02,
	0x15, 0x00,
	0x26, 0xFF, 0x00,
	0x75, 0x08,
	0x95, 0x3F,             /* Report Count (63) */
	0x81, 0x02,             /* Input (Data,Var,Abs) */
	0x85, F103_HID_REPORT_ID_OUT,
	0x09, 0x03,
	0x15, 0x00,
	0x26, 0xFF, 0x00,
	0x75, 0x08,
	0x95, 0x3F,             /* Report Count (63) */
	0x91, 0x02,             /* Output (Data,Var,Abs) */
	0xC0,                   /* End Collection */
};

#endif /* F103_HID_DESCRIPTOR_H_ */

