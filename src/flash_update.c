/*
 * Interrupt-safe A/B updater for STM32F103CBT6.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "flash_update.h"
#include "f103_iap.h"

#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#define FLASH_DEVICE DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller))

#define SLOT_A 0
#define SLOT_B 1
#define SLOT_COUNT 2
#define META_CRC_OFFSET offsetof(struct f103_meta, record_crc)

BUILD_ASSERT((F103_SLOT_A_OFFSET % F103_FLASH_PAGE_SIZE) == 0U,
	     "Slot A must be page aligned");
BUILD_ASSERT((F103_SLOT_B_OFFSET % F103_FLASH_PAGE_SIZE) == 0U,
	     "Slot B must be page aligned");
BUILD_ASSERT((F103_SLOT_SIZE % F103_FLASH_PAGE_SIZE) == 0U,
	     "Slot size must be a multiple of the flash page size");
BUILD_ASSERT(F103_SLOT_A_OFFSET + F103_SLOT_SIZE == F103_SLOT_B_OFFSET,
	     "Slots must be contiguous");
BUILD_ASSERT(F103_SLOT_B_OFFSET + F103_SLOT_SIZE == F103_META_A_OFFSET,
	     "Slot B must end before metadata");
BUILD_ASSERT(F103_META_A_OFFSET + F103_META_PAGE_SIZE == F103_META_B_OFFSET,
	     "Metadata pages must be contiguous");
BUILD_ASSERT(F103_FLASH_BASE + F103_META_B_OFFSET + F103_META_PAGE_SIZE ==
		     F103_FLASH_END,
	     "Metadata must end at the end of flash");
BUILD_ASSERT(sizeof(struct f103_meta) == 60U,
	     "Unexpected metadata structure layout");

struct update_context {
	bool receiving;
	uint8_t target_slot;
	uint32_t image_size;
	uint32_t bytes_received;
	uint32_t running_crc;
	uint32_t page_offset;
	uint32_t page_used;
	uint8_t page_buffer[F103_FLASH_PAGE_SIZE];
};

static struct update_context ctx;

static uint32_t slot_base(uint8_t slot)
{
	return slot == SLOT_A ? F103_SLOT_A_BASE : F103_SLOT_B_BASE;
}

static uint32_t slot_offset(uint8_t slot)
{
	return slot == SLOT_A ? F103_SLOT_A_OFFSET : F103_SLOT_B_OFFSET;
}

static uint32_t meta_offset(uint8_t page)
{
	return page == 0U ? F103_META_A_OFFSET : F103_META_B_OFFSET;
}

static bool metadata_record_valid(struct f103_meta *meta)
{
	const uint32_t calc_crc =
		crc32_ieee((const uint8_t *)meta, META_CRC_OFFSET);

	if (meta->magic != F103_META_MAGIC ||
	    meta->sequence == 0U ||
	    meta->active_slot >= SLOT_COUNT ||
	    meta->known_good_slot >= SLOT_COUNT ||
	    calc_crc != meta->record_crc) {
		return false;
	}

	for (uint8_t slot = 0U; slot < SLOT_COUNT; slot++) {
		const struct f103_slot_meta *info = &meta->slot[slot];

		if (info->state != F103_SLOT_VALID) {
			continue;
		}
		if (info->start != slot_base(slot) ||
		    info->size == 0U ||
		    info->size > F103_SLOT_SIZE) {
			return false;
		}
	}

	if (meta->slot[meta->active_slot].state != F103_SLOT_VALID ||
	    meta->slot[meta->known_good_slot].state != F103_SLOT_VALID) {
		return false;
	}

	if ((meta->flags & ~F103_META_FLAG_PENDING) != 0U) {
		return false;
	}

	if ((meta->flags & F103_META_FLAG_PENDING) != 0U) {
		return meta->boot_attempts <= F103_TEST_BOOT_MAX_ATTEMPTS;
	}

	return meta->boot_attempts == 0U;
}

static int read_meta_page(uint8_t page, struct f103_meta *meta)
{
	int err = flash_read(FLASH_DEVICE, meta_offset(page), meta,
			     sizeof(*meta));

	if (err < 0) {
		return err;
	}

	return metadata_record_valid(meta) ? 0 : -EINVAL;
}

static int find_latest_meta(struct f103_meta *meta, uint8_t *page)
{
	struct f103_meta candidate;
	bool found = false;

	for (uint8_t index = 0U; index < 2U; index++) {
		if (read_meta_page(index, &candidate) < 0) {
			continue;
		}

		if (!found || candidate.sequence > meta->sequence) {
			*meta = candidate;
			*page = index;
			found = true;
		}
	}

	return found ? 0 : -ENOENT;
}

static int verify_slot_image(const struct f103_slot_meta *info,
			     uint8_t slot, bool verify_crc)
{
	uint32_t vector[2];
	uint32_t offset = slot_offset(slot);
	uint32_t remaining = info->size;
	uint32_t crc = 0U;
	uint8_t read_buffer[256];

	if (info->state != F103_SLOT_VALID ||
	    info->start != slot_base(slot) ||
	    info->size == 0U ||
	    info->size > F103_SLOT_SIZE) {
		return -EINVAL;
	}

	if (flash_read(FLASH_DEVICE, offset, vector, sizeof(vector)) < 0) {
		return -EIO;
	}

	if (vector[0] < 0x20000000U || vector[0] > 0x20005000U ||
	    (vector[0] & 0x3U) != 0U ||
	    (vector[1] & 1U) == 0U ||
	    vector[1] < info->start ||
	    vector[1] >= info->start + info->size) {
		return -EINVAL;
	}

	if (!verify_crc) {
		return 0;
	}

	while (remaining > 0U) {
		const size_t chunk = MIN(remaining, (uint32_t)sizeof(read_buffer));

		if (flash_read(FLASH_DEVICE, offset, read_buffer, chunk) < 0) {
			return -EIO;
		}
		crc = crc32_ieee_update(crc, read_buffer, chunk);
		offset += chunk;
		remaining -= chunk;
	}

	return crc == info->crc32 ? 0 : -EILSEQ;
}

static int find_boot_slot(uint8_t *slot, bool verify_crc)
{
	struct f103_meta meta;
	uint8_t unused_page = 0U;

	if (find_latest_meta(&meta, &unused_page) < 0) {
		return -ENOENT;
	}

	const uint8_t active = meta.active_slot;
	if (verify_slot_image(&meta.slot[active], active, verify_crc) == 0) {
		*slot = active;
		return 0;
	}

	const uint8_t fallback = active ^ 1U;
	if (verify_slot_image(&meta.slot[fallback], fallback, verify_crc) == 0) {
		*slot = fallback;
		return 0;
	}

	return -ENOENT;
}

static uint8_t select_target_slot(void)
{
	struct f103_meta meta;
	uint8_t unused_page = 0U;

	if (find_latest_meta(&meta, &unused_page) < 0) {
		return SLOT_A;
	}

	const uint8_t active = meta.active_slot;

	if ((meta.flags & F103_META_FLAG_PENDING) != 0U) {
		/* Overwrite the unconfirmed test image, keep the fallback. */
		return active;
	}

	if (verify_slot_image(&meta.slot[active], active, true) == 0) {
		return active ^ 1U;
	}

	/* Repair the active slot instead of overwriting a possible fallback. */
	return active;
}

static int write_page(void)
{
	int err;

	if (ctx.page_used == 0U) {
		return 0;
	}

	if (ctx.page_used < sizeof(ctx.page_buffer)) {
		memset(&ctx.page_buffer[ctx.page_used], 0xFF,
		       sizeof(ctx.page_buffer) - ctx.page_used);
	}

	err = flash_erase(FLASH_DEVICE, ctx.page_offset,
			  sizeof(ctx.page_buffer));
	if (err < 0) {
		return err;
	}

	err = flash_write(FLASH_DEVICE, ctx.page_offset, ctx.page_buffer,
			  sizeof(ctx.page_buffer));
	if (err < 0) {
		return err;
	}

	ctx.page_offset += sizeof(ctx.page_buffer);
	ctx.page_used = 0U;
	return 0;
}

int flash_update_begin(uint32_t image_size)
{
	if (image_size == 0U || image_size > F103_SLOT_SIZE) {
		return -EINVAL;
	}

	ctx.receiving = false;
	ctx.target_slot = select_target_slot();
	ctx.image_size = image_size;
	ctx.bytes_received = 0U;
	ctx.running_crc = 0U;
	ctx.page_offset = slot_offset(ctx.target_slot);
	ctx.page_used = 0U;
	memset(ctx.page_buffer, 0xFF, sizeof(ctx.page_buffer));

	/*
	 * The active metadata and active image are deliberately left intact.
	 * Until a new metadata record is committed, power loss simply boots
	 * the previously active slot.
	 */
	ctx.receiving = true;
	return 0;
}

int flash_update_write(uint32_t offset, const uint8_t *data, size_t len)
{
	size_t copied = 0U;
	int err;

	if (!ctx.receiving || data == NULL) {
		return -EINVAL;
	}

	if (offset != ctx.bytes_received ||
	    len > (ctx.image_size - ctx.bytes_received)) {
		return -ERANGE;
	}

	ctx.running_crc = crc32_ieee_update(ctx.running_crc, data, len);

	while (copied < len) {
		const size_t space = sizeof(ctx.page_buffer) - ctx.page_used;
		const size_t chunk = MIN(space, len - copied);

		memcpy(&ctx.page_buffer[ctx.page_used], &data[copied], chunk);
		ctx.page_used += chunk;
		ctx.bytes_received += chunk;
		copied += chunk;

		if (ctx.page_used == sizeof(ctx.page_buffer)) {
			err = write_page();
			if (err < 0) {
				ctx.receiving = false;
				return err;
			}
		}
	}

	return 0;
}

static int verify_flash_crc(uint32_t expected_crc)
{
	uint32_t offset = slot_offset(ctx.target_slot);
	uint32_t remaining = ctx.image_size;
	uint32_t crc = 0U;

	while (remaining > 0U) {
		const size_t chunk = MIN(remaining,
					 (uint32_t)sizeof(ctx.page_buffer));
		int err;

		err = flash_read(FLASH_DEVICE, offset, ctx.page_buffer, chunk);
		if (err < 0) {
			return err;
		}

		crc = crc32_ieee_update(crc, ctx.page_buffer, chunk);
		offset += chunk;
		remaining -= chunk;
	}

	return (crc == expected_crc) ? 0 : -EIO;
}

static int write_meta_record(uint8_t page_index, const struct f103_meta *meta)
{
	struct f103_meta record = *meta;

	record.magic = F103_META_MAGIC;
	record.record_crc = crc32_ieee((const uint8_t *)&record, META_CRC_OFFSET);

	int err = flash_erase(FLASH_DEVICE, meta_offset(page_index),
			      F103_META_PAGE_SIZE);
	if (err < 0) {
		return err;
	}

	err = flash_write(FLASH_DEVICE, meta_offset(page_index),
			  &record, META_CRC_OFFSET);
	if (err < 0) {
		return err;
	}

	/*
	 * Program record_crc separately and last. An interrupted commit
	 * therefore leaves the previous metadata page authoritative.
	 */
	return flash_write(FLASH_DEVICE,
			   meta_offset(page_index) + META_CRC_OFFSET,
			   &record.record_crc, sizeof(record.record_crc));
}

/*
 * Commit the freshly downloaded image as a pending TEST record instead of
 * switching the active slot permanently. The previous valid slot is kept
 * as known_good_slot so the bootloader can revert on failure.
 */
static int commit_pending_metadata(uint32_t image_crc)
{
	struct f103_meta meta;
	uint8_t old_page = 0U;
	uint8_t write_page_index;
	int err = find_latest_meta(&meta, &old_page);

	if (err < 0) {
		memset(&meta, 0, sizeof(meta));
		meta.sequence = 0U;
		meta.known_good_slot = ctx.target_slot;
		write_page_index = 0U;
	} else {
		write_page_index = old_page ^ 1U;
		if ((meta.flags & F103_META_FLAG_PENDING) == 0U) {
			meta.known_good_slot = meta.active_slot;
		}
	}

	meta.sequence++;
	meta.active_slot = ctx.target_slot;
	meta.slot[ctx.target_slot].start = slot_base(ctx.target_slot);
	meta.slot[ctx.target_slot].size = ctx.image_size;
	meta.slot[ctx.target_slot].crc32 = image_crc;
	meta.slot[ctx.target_slot].state = F103_SLOT_VALID;
	meta.flags = F103_META_FLAG_PENDING;
	meta.boot_attempts = 0U;

	return write_meta_record(write_page_index, &meta);
}

/*
 * Decide what to do with a pending TEST record before booting:
 *
 * - healthy image and attempts left: persist attempts + 1, then test boot
 * - broken image or attempts exhausted with a fallback: revert to it
 * - attempts exhausted without any fallback: auto-confirm instead of
 *   bricking a device that only ever had this one image
 */
static bool handle_pending_state(struct f103_meta *meta, uint8_t *page)
{
	if ((meta->flags & F103_META_FLAG_PENDING) == 0U) {
		return true;
	}

	const uint8_t active = meta->active_slot;
	const uint8_t good = meta->known_good_slot;
	const bool image_ok =
		verify_slot_image(&meta->slot[active], active, true) == 0;
	const bool has_fallback = good != active;

	if (image_ok && meta->boot_attempts < F103_TEST_BOOT_MAX_ATTEMPTS) {
		meta->sequence++;
		meta->boot_attempts++;
		if (write_meta_record(*page ^ 1U, meta) < 0) {
			return false;
		}
		*page ^= 1U;
		return true;
	}

	if (!has_fallback) {
		if (!image_ok) {
			return false;
		}
		/* No fallback: promote the healthy test image permanently. */
	} else {
		if (verify_slot_image(&meta->slot[good], good, true) != 0) {
			return false;
		}
		meta->active_slot = good;
	}

	meta->sequence++;
	meta->flags = 0U;
	meta->boot_attempts = 0U;
	if (write_meta_record(*page ^ 1U, meta) < 0) {
		return false;
	}
	*page ^= 1U;
	return true;
}

int flash_update_finish(uint32_t expected_crc, uint32_t *actual_crc)
{
	int err;

	if (!ctx.receiving || ctx.bytes_received != ctx.image_size) {
		return -EINVAL;
	}

	err = write_page();
	if (err < 0) {
		ctx.receiving = false;
		return err;
	}

	ctx.receiving = false;
	if (actual_crc != NULL) {
		*actual_crc = ctx.running_crc;
	}

	if (ctx.running_crc != expected_crc) {
		return -EILSEQ;
	}

	err = verify_flash_crc(expected_crc);
	if (err < 0) {
		return err;
	}

	return commit_pending_metadata(ctx.running_crc);
}

bool flash_update_image_valid(bool verify_crc)
{
	uint8_t slot;

	return find_boot_slot(&slot, verify_crc) == 0;
}

bool flash_update_get_boot_info(uint32_t *base, bool verify_crc)
{
	struct f103_meta meta;
	uint8_t page = 0U;

	if (find_latest_meta(&meta, &page) < 0) {
		return false;
	}

	if (!handle_pending_state(&meta, &page)) {
		return false;
	}

	const uint8_t active = meta.active_slot;
	if (verify_slot_image(&meta.slot[active], active, verify_crc) != 0) {
		return false;
	}

	if (base != NULL) {
		*base = slot_base(active);
	}
	return true;
}

int flash_update_active_slot(void)
{
	uint8_t slot;

	return find_boot_slot(&slot, false) == 0 ? slot : -ENOENT;
}

int flash_update_target_slot(void)
{
	return ctx.receiving ? ctx.target_slot : select_target_slot();
}

uint32_t flash_update_slot_base(int slot)
{
	if (slot < 0 || slot >= SLOT_COUNT) {
		return 0U;
	}
	return slot_base((uint8_t)slot);
}

uint32_t flash_update_slot_size(void)
{
	return F103_SLOT_SIZE;
}


bool flash_update_test_state(uint8_t *pending, uint8_t *attempts,
                             uint8_t *known_good_slot)
{
	struct f103_meta meta;
	uint8_t page = 0U;

	if (find_latest_meta(&meta, &page) < 0) {
		return false;
	}

	if (pending != NULL) {
		*pending = ((meta.flags & F103_META_FLAG_PENDING) != 0U) ? 1U : 0U;
	}
	if (attempts != NULL) {
		*attempts = (uint8_t)meta.boot_attempts;
	}
	if (known_good_slot != NULL) {
		*known_good_slot = meta.known_good_slot;
	}
	return true;
}

int flash_update_confirm_image(void)
{
	struct f103_meta meta;
	uint8_t page = 0U;

	if (find_latest_meta(&meta, &page) < 0) {
		return -ENOENT;
	}

	if ((meta.flags & F103_META_FLAG_PENDING) == 0U) {
		return 0;
	}

	meta.sequence++;
	meta.flags = 0U;
	meta.boot_attempts = 0U;
	meta.known_good_slot = meta.active_slot;

	return write_meta_record(page ^ 1U, &meta);
}
