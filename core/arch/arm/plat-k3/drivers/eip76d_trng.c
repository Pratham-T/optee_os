// SPDX-License-Identifier: BSD-2-Clause
/*
 * Texas Instruments K3 EIP76D TRNG Driver
 *
 * Copyright (C) 2022 Texas Instruments Incorporated - https://www.ti.com/
 *	Andrew Davis <afd@ti.com>
 */

#include <initcall.h>
#include <io.h>
#include <keep.h>
#include <kernel/interrupt.h>
#include <kernel/misc.h>
#include <kernel/spinlock.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <rng_support.h>

#include "eip76d_trng.h"

#define	RNG_OUTPUT_0            0x00
#define	RNG_OUTPUT_1            0x04
#define	RNG_OUTPUT_2            0x08
#define	RNG_OUTPUT_3            0x0C
#define	RNG_STATUS              0x10
#define RNG_READY               BIT(0)
#define SHUTDOWN_OFLO           BIT(1)
#define	RNG_INTACK              0x10
#define	RNG_CONTROL             0x14
#define ENABLE_TRNG             BIT(10)
#define	RNG_CONFIG              0x18
#define	RNG_ALARMCNT            0x1C
#define	RNG_FROENABLE           0x20
#define	RNG_FRODETUNE           0x24
#define	RNG_ALARMMASK           0x28
#define	RNG_ALARMSTOP           0x2C
#define	RNG_OPTIONS             0x78
#define	RNG_EIP_REV             0x7C

#define RNG_CONTROL_STARTUP_CYCLES_SHIFT        16
#define RNG_CONTROL_STARTUP_CYCLES_MASK         GENMASK_32(31, 16)

#define RNG_CONFIG_MAX_REFIL_CYCLES_SHIFT       16
#define RNG_CONFIG_MAX_REFIL_CYCLES_MASK        GENMASK_32(31, 16)
#define RNG_CONFIG_MIN_REFIL_CYCLES_SHIFT       0
#define RNG_CONFIG_MIN_REFIL_CYCLES_MASK        GENMASK_32(7, 0)

#define RNG_ALARMCNT_ALARM_TH_SHIFT             0
#define RNG_ALARMCNT_ALARM_TH_MASK              GENMASK_32(7, 0)
#define RNG_ALARMCNT_SHUTDOWN_TH_SHIFT          16
#define RNG_ALARMCNT_SHUTDOWN_TH_MASK           GENMASK_32(20, 16)

#define RNG_CONTROL_STARTUP_CYCLES              0xff
#define RNG_CONFIG_MIN_REFIL_CYCLES             0x5
#define RNG_CONFIG_MAX_REFIL_CYCLES             0x22
#define RNG_ALARM_THRESHOLD                     0xff
#define RNG_SHUTDOWN_THRESHOLD                  0x4

#define RNG_FRO_MASK    GENMASK_32(23, 0)

register_phys_mem_pgdir(MEM_AREA_IO_SEC, RNG_BASE, RNG_REG_SIZE);

static struct mutex fifo_lock = MUTEX_INITIALIZER;
static vaddr_t rng;

static bool eip76d_rng_is_enabled(void)
{
	return io_read32(rng + RNG_CONTROL) & ENABLE_TRNG;
}

static void eip76d_rng_init_seq(void)
{
	uint32_t val = 0;

	/* Ensure initial latency */
	val |= RNG_CONFIG_MIN_REFIL_CYCLES << RNG_CONFIG_MIN_REFIL_CYCLES_SHIFT;
	val |= RNG_CONFIG_MAX_REFIL_CYCLES << RNG_CONFIG_MAX_REFIL_CYCLES_SHIFT;
	io_write32(rng + RNG_CONFIG, val);

	/* Configure the desired FROs */
	io_write32(rng + RNG_FRODETUNE, 0x0);

	/* Enable all FROs */
	io_write32(rng + RNG_FROENABLE, 0xffffff);

	io_write32(rng + RNG_CONTROL, ENABLE_TRNG);
}

static void eip76d_rng_read128(uint32_t *word0, uint32_t *word1,
			       uint32_t *word2, uint32_t *word3)
{
	if (!eip76d_rng_is_enabled())
		eip76d_rng_init_seq();

	/* Is the result ready (available)? */
	while (!(io_read32(rng + RNG_STATUS) & RNG_READY)) {
		/* Is the shutdown threshold reached? */
		if (io_read32(rng + RNG_STATUS) & SHUTDOWN_OFLO) {
			uint32_t alarm = io_read32(rng + RNG_ALARMSTOP);
			uint32_t tune = io_read32(rng + RNG_FRODETUNE);

			/* Clear the alarm events */
			io_write32(rng + RNG_ALARMMASK, 0x0);
			io_write32(rng + RNG_ALARMSTOP, 0x0);
			/* De-tune offending FROs */
			io_write32(rng + RNG_FRODETUNE, tune ^ alarm);
			/* Re-enable the shut down FROs */
			io_write32(rng + RNG_FROENABLE, RNG_FRO_MASK);
			/* Clear the shutdown overflow event */
			io_write32(rng + RNG_INTACK, SHUTDOWN_OFLO);

			DMSG("Fixed FRO shutdown");
		}
	}
	/* Read random value */
	*word0 = io_read32(rng + RNG_OUTPUT_0);
	*word1 = io_read32(rng + RNG_OUTPUT_1);
	*word2 = io_read32(rng + RNG_OUTPUT_2);
	*word3 = io_read32(rng + RNG_OUTPUT_3);
	/* Acknowledge read complete */
	io_write32(rng + RNG_INTACK, RNG_READY);
}

TEE_Result hw_get_random_bytes(void *buf, size_t len)
{
	static union {
		uint32_t val[4];
		uint8_t byte[16];
	} fifo;
	static size_t fifo_pos;
	uint8_t *buffer = buf;
	size_t buffer_pos = 0;

	while (buffer_pos < len) {
		mutex_lock(&fifo_lock);

		/* Refill our FIFO */
		if (fifo_pos == 0)
			eip76d_rng_read128(&fifo.val[0], &fifo.val[1],
					   &fifo.val[2], &fifo.val[3]);

		buffer[buffer_pos++] = fifo.byte[fifo_pos++];
		fifo_pos %= 16;

		mutex_unlock(&fifo_lock);
	}

	return TEE_SUCCESS;
}

TEE_Result eip76d_rng_init(void)
{
	rng = (vaddr_t)phys_to_virt(RNG_BASE, MEM_AREA_IO_SEC, RNG_REG_SIZE);

	eip76d_rng_init_seq();

	IMSG("EIP76D TRNG initialized");

	return TEE_SUCCESS;
}
