/*
 * STM32F103 backup-domain boot request.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "f103_bootloader_api.h"
#include "f103_iap.h"
#include "flash_update.h"

#include <stm32f1xx.h>
#include <zephyr/arch/cpu.h>

static void backup_domain_enable(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
	(void)RCC->APB1ENR;

	PWR->CR |= PWR_CR_DBP;
	__DSB();
}

void f103_bootloader_mark_request(void)
{
	backup_domain_enable();
	BKP->DR1 = F103_BOOT_REQUEST_MAGIC;
	__DSB();
}

void f103_bootloader_request(void)
{
	f103_bootloader_mark_request();
	NVIC_SystemReset();

	__builtin_unreachable();
}

bool f103_bootloader_consume_request(void)
{
	backup_domain_enable();

	const bool requested = (BKP->DR1 == F103_BOOT_REQUEST_MAGIC);

	BKP->DR1 = 0U;
	__DSB();

	return requested;
}


bool f103_bootloader_confirm_image(void)
{
	return flash_update_confirm_image() == 0;
}
