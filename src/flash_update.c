/*
 * Interrupt-safe A/B updater with MCUboot-style sector swap for the
 * STM32F103CBT6.
 *
 * Firmware is always downloaded into slot B (download slot). The boot
 * decision verifies the candidate, exchanges slot A and slot B sector by
 * sector through a scratch sector (each step persisted in a swap log) and
 * boots the new image from slot A in TEST mode. The running application
 * confirms with image_ok; otherwise the bootloader swaps back.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "flash_update.h"
#include "f103_iap.h"

#include <stddef.h>
#include <string.h>

#include <stm32f1xx.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#define FLASH_DEVICE DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller))

static int raw_erase_page(uint32_t offset);
static int raw_flash_write(uint32_t offset, const void *data, uint32_t len);

#define SLOT_PRIMARY   0U /* slot A: always the boot slot */
#define SLOT_SECONDARY 1U /* slot B: always the download slot */
#define SLOT_COUNT     2U
#define META_CRC_OFFSET offsetof(struct f103_meta, record_crc)
#define SWAP_SECTORS (F103_SLOT_SIZE / F103_FLASH_PAGE_SIZE)

BUILD_ASSERT((F103_SLOT_A_OFFSET % F103_FLASH_PAGE_SIZE) == 0U,
	     "Slot A must be page aligned");
BUILD_ASSERT((F103_SCRATCH_OFFSET % F103_FLASH_PAGE_SIZE) == 0U,
	     "Scratch must be page aligned");
BUILD_ASSERT((F103_SLOT_B_OFFSET % F103_FLASH_PAGE_SIZE) == 0U,
	     "Slot B must be page aligned");
BUILD_ASSERT((F103_SWAP_LOG_OFFSET % F103_FLASH_PAGE_SIZE) == 0U,
	     "Swap log must be page aligned");
BUILD_ASSERT(F103_SLOT_A_OFFSET + F103_SLOT_SIZE == F103_SCRATCH_OFFSET,
	     "Slot A must be followed by the scratch sector");
BUILD_ASSERT(F103_SCRATCH_OFFSET + F103_FLASH_PAGE_SIZE == F103_SLOT_B_OFFSET,
	     "Scratch must be followed by slot B");
BUILD_ASSERT(F103_SLOT_B_OFFSET + F103_SLOT_SIZE == F103_SWAP_LOG_OFFSET,
	     "Slot B must be followed by the swap log");
BUILD_ASSERT(F103_SWAP_LOG_OFFSET + F103_FLASH_PAGE_SIZE == F103_META_A_OFFSET,
	     "Swap log must be followed by metadata");
BUILD_ASSERT(F103_META_A_OFFSET + F103_META_PAGE_SIZE == F103_META_B_OFFSET,
	     "Metadata pages must be contiguous");
BUILD_ASSERT(F103_FLASH_BASE + F103_META_B_OFFSET + F103_META_PAGE_SIZE ==
		     F103_FLASH_END,
	     "Metadata must end at the end of flash");
BUILD_ASSERT(sizeof(struct f103_meta) == 60U,
	     "Unexpected metadata structure layout");
BUILD_ASSERT((SWAP_SECTORS * sizeof(uint16_t)) <= F103_FLASH_PAGE_SIZE,
	     "Swap log does not fit into one page");

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
	return slot == SLOT_PRIMARY ? F103_SLOT_A_BASE : F103_SLOT_B_BASE;
}

static uint32_t slot_offset(uint8_t slot)
{
	return slot == SLOT_PRIMARY ? F103_SLOT_A_OFFSET : F103_SLOT_B_OFFSET;
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
	    meta->active_slot != SLOT_PRIMARY ||
	    meta->known_good_slot >= SLOT_COUNT ||
	    calc_crc != meta->record_crc) {
		return false;
	}

	for (uint8_t slot = 0U; slot < SLOT_COUNT; slot++) {
		const struct f103_slot_meta *info = &meta->slot[slot];

		if (info->state != F103_SLOT_VALID) {
			/* Slot B may be empty before the first download. */
			if (slot == SLOT_PRIMARY || info->state != 0U) {
				return false;
			}
			continue;
		}
		if (info->start != slot_base(slot) ||
		    info->size == 0U ||
		    info->size > F103_SLOT_SIZE) {
			return false;
		}
	}

	if (meta->slot[meta->known_good_slot].state != F103_SLOT_VALID) {
		return false;
	}

	if ((meta->flags & ~(F103_META_FLAG_PENDING | F103_META_FLAG_SWAP)) !=
	    0U) {
		return false;
	}

	if ((meta->flags & F103_META_FLAG_SWAP) != 0U) {
		return meta->known_good_slot == SLOT_PRIMARY;
	}
	if ((meta->flags & F103_META_FLAG_PENDING) != 0U) {
		return meta->known_good_slot == SLOT_SECONDARY &&
		       meta->boot_attempts <= F103_TEST_BOOT_MAX_ATTEMPTS;
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
		const int rc = read_meta_page(index, &candidate);

		if (rc < 0) {
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

static int write_meta_record(uint8_t page_index, const struct f103_meta *meta)
{
	struct f103_meta record = *meta;

	record.magic = F103_META_MAGIC;
	record.record_crc = crc32_ieee((const uint8_t *)&record, META_CRC_OFFSET);

	int err = 0;

	for (uint32_t attempt = 0U; attempt < 3U; attempt++) {
		err = raw_erase_page(meta_offset(page_index));
		if (err == 0) {
			err = raw_flash_write(meta_offset(page_index),
					      &record, META_CRC_OFFSET);
		}
		if (err == 0) {
			err = raw_flash_write(meta_offset(page_index) + META_CRC_OFFSET,
					      &record.record_crc,
					      sizeof(record.record_crc));
		}
		if (err != 0) {
			continue;
		}

		/* Read back and verify the record actually stuck. */
		struct f103_meta check;

		if (flash_read(FLASH_DEVICE, meta_offset(page_index),
			       &check, sizeof(check)) < 0) {
			err = -EIO;
			continue;
		}
		if (check.sequence != record.sequence ||
		    check.flags != record.flags ||
		    check.record_crc != record.record_crc) {
			err = -EIO;
			continue;
		}

		return 0;
	}

	return err;
}

/*
 * Verify an image descriptor. Images are single-linked for slot A, so the
 * vector table must always point into slot A even while the image is
 * stored in slot B waiting for the swap.
 */
static int verify_slot_image(const struct f103_slot_meta *info,
			     uint8_t storage_slot, bool verify_crc)
{
	uint32_t vector[2];
	uint32_t offset = slot_offset(storage_slot);
	uint32_t remaining = info->size;
	uint32_t crc = 0U;
	uint8_t read_buffer[256];

	if (info->state != F103_SLOT_VALID ||
	    info->start != slot_base(storage_slot) ||
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
	    vector[1] < F103_SLOT_A_BASE ||
	    vector[1] >= F103_SLOT_A_BASE + info->size) {
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
	uint8_t page = 0U;

	if (find_latest_meta(&meta, &page) < 0) {
		return -ENOENT;
	}

	if (verify_slot_image(&meta.slot[SLOT_PRIMARY], SLOT_PRIMARY,
			      verify_crc) != 0) {
		return -EINVAL;
	}

	*slot = SLOT_PRIMARY;
	return 0;
}

static uint32_t crc_region(uint32_t offset, uint32_t len)
{
	uint32_t crc = 0U;
	uint8_t read_buffer[256];

	while (len > 0U) {
		const size_t chunk = MIN(len, (uint32_t)sizeof(read_buffer));

		if (flash_read(FLASH_DEVICE, offset, read_buffer, chunk) < 0) {
			return 0U;
		}
		crc = crc32_ieee_update(crc, read_buffer, chunk);
		offset += chunk;
		len -= chunk;
	}

	return crc;
}

static int copy_region(uint32_t src_off, uint32_t dst_off, uint32_t len)
{
	uint8_t read_buffer[256];

	while (len > 0U) {
		const size_t chunk = MIN(len, (uint32_t)sizeof(read_buffer));

		if (flash_read(FLASH_DEVICE, src_off, read_buffer, chunk) < 0) {
			return -EIO;
		}
		if (flash_write(FLASH_DEVICE, dst_off, read_buffer, chunk) < 0) {
			return -EIO;
		}
		src_off += chunk;
		dst_off += chunk;
		len -= chunk;
	}

	return 0;
}

/*
 * Sector exchange with an append-only swap log.
 *
 * F1 flash words can only be programmed once (0xFFFF -> value), so the
 * log is written strictly sequentially: each step appends one 16-bit
 * entry (value = previous + 1) at the next blank position. Three entries
 * per sector create power-loss safe checkpoints:
 *
 *   entry 2k+1: A[k] copied to scratch, A[k] erased+replaced by B[k]
 *               (scratch holds the original A[k] data)
 *   entry 2k+2: B[k] erased and restored from scratch (sector k done)
 *   entry 2k+3: scratch refreshed for sector k+1
 *
 * After a reset the log is replayed: a trailing odd count finishes the
 * interrupted sector k by restoring B[k] from the still-valid scratch,
 * then the exchange continues with sector k+1.
 */
#define SWAP_LOG_ENTRIES (SWAP_SECTORS * 3U)

/*
 * Register-level page erase for the swap context. Single-owner: the swap
 * runs from the boot decision with USB disabled and no other flash user.
 */
static int raw_erase_page(uint32_t offset)
{
	uint32_t timeout = 1000000U;

	if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
		FLASH->KEYR = 0x45670123u;
		FLASH->KEYR = 0xCDEF89ABu;
		if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
			return -EIO;
		}
	}

	/* Clear stale error flags (write 1 to clear) */
	FLASH->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;

	while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
		if (--timeout == 0U) {
			return -ETIMEDOUT;
		}
	}

	FLASH->CR |= FLASH_CR_PER;
	FLASH->AR = F103_FLASH_BASE + offset;
	FLASH->CR |= FLASH_CR_STRT;

	timeout = 1000000U;
	while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
		if (--timeout == 0U) {
			FLASH->CR &= ~FLASH_CR_PER;
			FLASH->CR |= FLASH_CR_LOCK;
			return -ETIMEDOUT;
		}
	}

	FLASH->CR &= ~FLASH_CR_PER;
	FLASH->CR |= FLASH_CR_LOCK;
	return 0;
}

/*
 * Register-level buffer write (half-word programming). Same rationale as
 * raw_erase_page: bypasses the generic driver inside the swap context.
 */
static int raw_flash_write(uint32_t offset, const void *data, uint32_t len)
{
	const uint16_t *src = (const uint16_t *)data;
	const uint32_t words = len / 2U;
	volatile uint16_t *dst = (volatile uint16_t *)(F103_FLASH_BASE + offset);

	if ((len % 2U) != 0U) {
		return -EINVAL;
	}

	if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
		FLASH->KEYR = 0x45670123u;
		FLASH->KEYR = 0xCDEF89ABu;
		if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
			return -EIO;
		}
	}

	FLASH->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;

	while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
	}

	FLASH->CR |= FLASH_CR_PG;

	for (uint32_t i = 0U; i < words; i++) {
		if (dst[i] != 0xFFFFU) {
			FLASH->CR &= ~FLASH_CR_PG;
			FLASH->CR |= FLASH_CR_LOCK;
			return -EIO;
		}
		dst[i] = src[i];
		while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
		}
		if ((FLASH->SR & FLASH_SR_PGERR) != 0U) {
			FLASH->SR = FLASH_SR_PGERR;
			FLASH->CR &= ~FLASH_CR_PG;
			FLASH->CR |= FLASH_CR_LOCK;
			return -EIO;
		}
	}

	FLASH->CR &= ~FLASH_CR_PG;
	FLASH->CR |= FLASH_CR_LOCK;
	return 0;
}

static int swap_log_count(void)
{
	uint32_t count = 0U;

	for (uint32_t i = 0U; i < SWAP_LOG_ENTRIES; i++) {
		uint16_t value = 0U;

		if (flash_read(FLASH_DEVICE,
			       F103_SWAP_LOG_OFFSET + i * sizeof(uint16_t),
			       &value, sizeof(value)) < 0) {
			return -EIO;
		}
		if (value == 0xFFFFU) {
			break;
		}
		if (value != (uint16_t)(i + 1U)) {
			return -EINVAL;
		}
		count = i + 1U;
	}

	return (int)count;
}

static int swap_log_append(uint32_t count)
{
	uint16_t value = (uint16_t)(count + 1U);

	return flash_write(FLASH_DEVICE,
			   F103_SWAP_LOG_OFFSET + count * sizeof(uint16_t),
			   &value, sizeof(value));
}

static int do_swap(struct f103_meta *meta, uint8_t page)
{
	const uint32_t scratch_off = F103_SCRATCH_OFFSET;
	int count = swap_log_count();

	if (count < 0) {
		return count;
	}

	uint32_t done = (uint32_t)count / 3U;
	const uint32_t rem = (uint32_t)count % 3U;

	if (rem == 1U) {
		/*
		 * Entry 3k+1 is present: scratch holds the original A[k], but
		 * A[k] may have been erased or partially rewritten. B[k] is
		 * still intact, so redo the A-side copy and then restore B.
		 */
		const uint32_t a_off =
			F103_SLOT_A_OFFSET + done * F103_FLASH_PAGE_SIZE;
		const uint32_t b_off =
			F103_SLOT_B_OFFSET + done * F103_FLASH_PAGE_SIZE;

		if (raw_erase_page(a_off) < 0 ||
		    copy_region(b_off, a_off, F103_FLASH_PAGE_SIZE) < 0) {
			return -EIO;
		}
		if (swap_log_append((uint32_t)count) < 0) {
			return -EIO;
		}
		count++;

		if (raw_erase_page(b_off) < 0 ||
		    copy_region(scratch_off, b_off, F103_FLASH_PAGE_SIZE) < 0) {
			return -EIO;
		}
		if (swap_log_append((uint32_t)count) < 0) {
			return -EIO;
		}
		count++;
		done++;
	} else if (rem == 2U) {
		/*
		 * Entry 3k+2 is present: A[k] already holds B's sector, B[k] is
		 * untouched and scratch still holds the original A[k]. Finish by
		 * restoring B[k].
		 */
		const uint32_t b_off =
			F103_SLOT_B_OFFSET + done * F103_FLASH_PAGE_SIZE;

		if (raw_erase_page(b_off) < 0 ||
		    copy_region(scratch_off, b_off, F103_FLASH_PAGE_SIZE) < 0) {
			return -EIO;
		}
		if (swap_log_append((uint32_t)count) < 0) {
			return -EIO;
		}
		count++;
		done++;
	}

	for (; done < SWAP_SECTORS; done++) {
		const uint32_t a_off = F103_SLOT_A_OFFSET + done * F103_FLASH_PAGE_SIZE;
		const uint32_t b_off = F103_SLOT_B_OFFSET + done * F103_FLASH_PAGE_SIZE;

		if (raw_erase_page(scratch_off) < 0 ||
		    copy_region(a_off, scratch_off, F103_FLASH_PAGE_SIZE) < 0) {
			return -EIO;
		}
		if (swap_log_append((uint32_t)count) < 0) {
			return -EIO;
		}
		count++;

		if (raw_erase_page(a_off) < 0 ||
		    copy_region(b_off, a_off, F103_FLASH_PAGE_SIZE) < 0) {
			return -EIO;
		}
		if (swap_log_append((uint32_t)count) < 0) {
			return -EIO;
		}
		count++;

		if (raw_erase_page(b_off) < 0 ||
		    copy_region(scratch_off, b_off, F103_FLASH_PAGE_SIZE) < 0) {
			return -EIO;
		}
		if (swap_log_append((uint32_t)count) < 0) {
			return -EIO;
		}
		count++;
	}

	const struct f103_slot_meta tmp = meta->slot[SLOT_PRIMARY];

	meta->slot[SLOT_PRIMARY] = meta->slot[SLOT_SECONDARY];
	meta->slot[SLOT_SECONDARY] = tmp;

	/*
	 * Descriptors travel with the image, but every descriptor must
	 * describe the slot it now lives in. Forgetting this fix-up makes the
	 * committed record fail validation and traps the bootloader in a
	 * do_swap loop.
	 */
	meta->slot[SLOT_PRIMARY].start = F103_SLOT_A_BASE;
	meta->slot[SLOT_SECONDARY].start = F103_SLOT_B_BASE;

	meta->known_good_slot ^= 1U;
	meta->flags &= ~F103_META_FLAG_SWAP;
	meta->sequence++;

	int err = write_meta_record(page ^ 1U, meta);

	if (err < 0) {
	}
	return err;
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
	ctx.target_slot = SLOT_SECONDARY;
	ctx.image_size = image_size;
	ctx.bytes_received = 0U;
	ctx.running_crc = 0U;
	ctx.page_offset = slot_offset(ctx.target_slot);
	ctx.page_used = 0U;
	memset(ctx.page_buffer, 0xFF, sizeof(ctx.page_buffer));

	/*
	 * Slot A and the metadata are deliberately left intact. Until the
	 * download record is committed, power loss simply boots the
	 * previously active image from slot A.
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

/*
 * Commit a freshly downloaded image as a swap request: candidate lives in
 * slot B, the exchange happens at the next boot decision. Erasing the swap
 * log here is safe because slot A and slot B are still in their
 * pre-swap state.
 */
static int commit_download_metadata(uint32_t image_crc)
{
	struct f103_meta meta;
	uint8_t old_page = 0U;
	uint8_t write_page_index;
	int err = find_latest_meta(&meta, &old_page);

	if (err < 0) {
		memset(&meta, 0, sizeof(meta));
		meta.sequence = 0U;
		write_page_index = 0U;
		/* Describe the image already living in slot A. */
		meta.slot[SLOT_PRIMARY].start = F103_SLOT_A_BASE;
		meta.slot[SLOT_PRIMARY].size = F103_SLOT_SIZE;
		meta.slot[SLOT_PRIMARY].crc32 =
			crc_region(F103_SLOT_A_OFFSET, F103_SLOT_SIZE);
		meta.slot[SLOT_PRIMARY].state = F103_SLOT_VALID;
	} else {
		write_page_index = old_page ^ 1U;
	}

	meta.sequence++;
	meta.active_slot = SLOT_PRIMARY;
	meta.known_good_slot = SLOT_PRIMARY;
	meta.boot_attempts = 0U;
	meta.slot[SLOT_SECONDARY].start = F103_SLOT_B_BASE;
	meta.slot[SLOT_SECONDARY].size = ctx.image_size;
	meta.slot[SLOT_SECONDARY].crc32 = image_crc;
	meta.slot[SLOT_SECONDARY].state = F103_SLOT_VALID;

	err = raw_erase_page(F103_SWAP_LOG_OFFSET);
	if (err < 0) {
		return err;
	}

	meta.flags = F103_META_FLAG_SWAP | F103_META_FLAG_PENDING;
	return write_meta_record(write_page_index, &meta);
}

/*
 * Commit a revert request: swap A and B back so the known-good image
 * returns to slot A. PENDING is cleared.
 */
static int commit_revert_request(struct f103_meta *meta, uint8_t page)
{
	int err;

	meta->sequence++;
	meta->active_slot = SLOT_PRIMARY;
	meta->known_good_slot = SLOT_PRIMARY;
	meta->boot_attempts = 0U;
	meta->flags = (meta->flags & ~F103_META_FLAG_PENDING) |
		      F103_META_FLAG_SWAP;

	err = raw_erase_page(F103_SWAP_LOG_OFFSET);
	if (err < 0) {
		return err;
	}

	return write_meta_record(page ^ 1U, meta);
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

	return commit_download_metadata(ctx.running_crc);
}

bool flash_update_image_valid(bool verify_crc)
{
	uint8_t slot;

	return find_boot_slot(&slot, verify_crc) == 0;
}

/*
 * Boot decision. Always boots slot A. Handles, in order:
 *   1. a requested A/B exchange (verify candidate in B, swap, persist)
 *   2. a pending TEST image (persist boot attempts, revert when exhausted)
 *   3. the normal confirmed image
 */
bool flash_update_get_boot_info(uint32_t *base, bool verify_crc)
{
	for (int pass = 0; pass < 8; pass++) {
		struct f103_meta meta;
		uint8_t page = 0U;

		if (find_latest_meta(&meta, &page) < 0) {
			return false;
		}

		if ((meta.flags & F103_META_FLAG_SWAP) != 0U) {
			/*
			 * Verify the candidate (or the image being swapped
			 * back) before touching any sector. A failure never
			 * switches the running image.
			 */
			if (verify_slot_image(&meta.slot[SLOT_SECONDARY],
					      SLOT_SECONDARY, true) != 0) {
				meta.sequence++;
				meta.flags = 0U;
				meta.boot_attempts = 0U;
				if (write_meta_record(page ^ 1U, &meta) < 0) {
					return false;
				}
				continue;
			}

			if (do_swap(&meta, page) < 0) {
				return false;
			}
			continue;
		}

		if ((meta.flags & F103_META_FLAG_PENDING) != 0U) {
			const bool image_ok =
				verify_slot_image(&meta.slot[SLOT_PRIMARY],
						  SLOT_PRIMARY,
						  true) == 0;
			const bool has_fallback =
				meta.known_good_slot == SLOT_SECONDARY &&
				meta.slot[SLOT_SECONDARY].state ==
					F103_SLOT_VALID;

			if (!image_ok) {
			}

			if (image_ok &&
			    meta.boot_attempts < F103_TEST_BOOT_MAX_ATTEMPTS) {
				meta.sequence++;
				meta.boot_attempts++;
				if (write_meta_record(page ^ 1U, &meta) < 0) {
					return false;
				}
				/* Fall through: TEST boot the image in A. */
			} else if (!image_ok || has_fallback) {
				/* Swap the known-good image back into A. */
				if (commit_revert_request(&meta, page) < 0) {
					return false;
				}
				continue;
			} else {
				/*
				 * Healthy test image without any fallback:
				 * promote it permanently instead of bricking.
				 */
				meta.sequence++;
				meta.flags &= ~F103_META_FLAG_PENDING;
				meta.boot_attempts = 0U;
				if (write_meta_record(page ^ 1U, &meta) < 0) {
					return false;
				}
			}
		}

		if (verify_slot_image(&meta.slot[SLOT_PRIMARY], SLOT_PRIMARY,
				      verify_crc) != 0) {
			return false;
		}

		if (base != NULL) {
			*base = F103_SLOT_A_BASE;
		}
		return true;
	}

	return false;
}

int flash_update_active_slot(void)
{
	struct f103_meta meta;
	uint8_t page = 0U;

	return find_latest_meta(&meta, &page) == 0 ? (int)SLOT_PRIMARY
						   : -ENOENT;
}

int flash_update_target_slot(void)
{
	return (int)SLOT_SECONDARY;
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
                             uint8_t *known_good_slot, uint8_t *swap_busy,
                             uint8_t *swap_done, uint8_t *swap_total)
{
	struct f103_meta meta;
	uint8_t page = 0U;

	if (find_latest_meta(&meta, &page) < 0) {
		return false;
	}

	if (pending != NULL) {
		*pending = ((meta.flags & F103_META_FLAG_PENDING) != 0U)
				   ? 1U
				   : 0U;
	}
	if (attempts != NULL) {
		*attempts = (uint8_t)meta.boot_attempts;
	}
	if (known_good_slot != NULL) {
		*known_good_slot = meta.known_good_slot;
	}
	if (swap_total != NULL) {
		*swap_total = (uint8_t)SWAP_SECTORS;
	}
	if (swap_busy != NULL || swap_done != NULL) {
		const int logged = (meta.flags & F103_META_FLAG_SWAP) != 0U
					   ? swap_log_count()
					   : 0;

		if (swap_busy != NULL) {
			*swap_busy = logged < 0 ? 1U : 0U;
		}
		if (swap_done != NULL) {
			*swap_done = logged > 0 ? (uint8_t)(logged / 3U) : 0U;
		}
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
	meta.flags &= ~F103_META_FLAG_PENDING;
	meta.boot_attempts = 0U;
	meta.known_good_slot = SLOT_PRIMARY;

	return write_meta_record(page ^ 1U, &meta);
}
