/*
 * Flash update backend for the F103 USB bootloader.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FLASH_UPDATE_H_
#define FLASH_UPDATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int flash_update_begin(uint32_t image_size);
int flash_update_write(uint32_t offset, const uint8_t *data, size_t len);
int flash_update_finish(uint32_t expected_crc, uint32_t *actual_crc);

bool flash_update_image_valid(bool verify_crc);
bool flash_update_get_boot_info(uint32_t *base, bool verify_crc);
int flash_update_active_slot(void);
int flash_update_target_slot(void);
uint32_t flash_update_slot_base(int slot);
uint32_t flash_update_slot_size(void);

/* Pending TEST / swap state of the latest metadata record. */
bool flash_update_test_state(uint8_t *pending, uint8_t *attempts,
                             uint8_t *known_good_slot, uint8_t *swap_busy,
                             uint8_t *swap_done, uint8_t *swap_total);

/*
 * Called by the running application after its self-test passed: clears the
 * pending flag (writes image_ok) and makes the tested image permanent.
 */
int flash_update_confirm_image(void);

#endif /* FLASH_UPDATE_H_ */
