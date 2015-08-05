/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) ARM Limited, 2014
 *
 * Authors:
 *  Jean-Philippe Brucker <jean-philippe.brucker@arm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/mmio.h>
#include <asm/irqchip.h>
#include <asm/processor.h>
#include <asm/smp.h>
#include <asm/traps.h>

unsigned int arch_mmio_count_regions(struct cell *cell)
{
	return smp_mmio_regions;
}

/* Taken from the ARM ARM pseudocode for taking a data abort */
static void arch_inject_dabt(struct trap_context *ctx, unsigned long addr)
{
	unsigned int lr_offset;
	unsigned long vbar;
	bool is_thumb;
	u32 sctlr, ttbcr;

	arm_read_sysreg(SCTLR_EL1, sctlr);
	arm_read_sysreg(TTBCR, ttbcr);

	/* Set cpsr */
	is_thumb = ctx->cpsr & PSR_T_BIT;
	ctx->cpsr &= ~(PSR_MODE_MASK | PSR_IT_MASK(0xff) | PSR_T_BIT
			| PSR_J_BIT | PSR_E_BIT);
	ctx->cpsr |= (PSR_ABT_MODE | PSR_I_BIT | PSR_A_BIT);
	if (sctlr & SCTLR_TE_BIT)
		ctx->cpsr |= PSR_T_BIT;
	if (sctlr & SCTLR_EE_BIT)
		ctx->cpsr |= PSR_E_BIT;

	lr_offset = (is_thumb ? 4 : 0);
	arm_write_banked_reg(LR_abt, ctx->pc + lr_offset);

	/* Branch to dabt vector */
	if (sctlr & SCTLR_V_BIT)
		vbar = 0xffff0000;
	else
		arm_read_sysreg(VBAR, vbar);
	ctx->pc = vbar + 0x10;

	/* Signal a debug fault. DFSR layout depends on the LPAE bit */
	if (ttbcr >> 31)
		arm_write_sysreg(DFSR, (1 << 9) | 0x22);
	else
		arm_write_sysreg(DFSR, 0x2);
	arm_write_sysreg(DFAR, addr);
}

void arm_mmio_perform_access(struct mmio_access *mmio)
{
	void *addr = (void *)mmio->address;

	if (mmio->is_write)
		switch (mmio->size) {
		case 1:
			mmio_write8(addr, mmio->value);
			return;
		case 2:
			mmio_write16(addr, mmio->value);
			return;
		case 4:
			mmio_write32(addr, mmio->value);
			return;
		}
	else
		switch (mmio->size) {
		case 1:
			mmio->value = mmio_read8(addr);
			return;
		case 2:
			mmio->value = mmio_read16(addr);
			return;
		case 4:
			mmio->value = mmio_read32(addr);
			return;
		}

	printk("WARNING: Ignoring unsupported MMIO access size %d\n",
	       mmio->size);
}

int arch_handle_dabt(struct trap_context *ctx)
{
	enum mmio_result mmio_result;
	struct mmio_access mmio;
	unsigned long hpfar;
	unsigned long hdfar;
	int ret		= TRAP_UNHANDLED;
	/* Decode the syndrome fields */
	u32 icc		= ESR_ICC(ctx->esr);
	u32 isv		= icc >> 24;
	u32 sas		= icc >> 22 & 0x3;
	u32 sse		= icc >> 21 & 0x1;
	u32 srt		= icc >> 16 & 0xf;
	u32 ea		= icc >> 9 & 0x1;
	u32 cm		= icc >> 8 & 0x1;
	u32 s1ptw	= icc >> 7 & 0x1;
	u32 is_write	= icc >> 6 & 0x1;
	u32 size	= 1 << sas;

	arm_read_sysreg(HPFAR, hpfar);
	arm_read_sysreg(HDFAR, hdfar);
	mmio.address = hpfar << 8;
	mmio.address |= hdfar & 0xfff;

	this_cpu_data()->stats[JAILHOUSE_CPU_STAT_VMEXITS_MMIO]++;

	/*
	 * Invalid instruction syndrome means multiple access or writeback, there
	 * is nothing we can do.
	 */
	if (!isv || size > sizeof(unsigned long))
		goto error_unhandled;

	/* Re-inject abort during page walk, cache maintenance or external */
	if (s1ptw || ea || cm) {
		arch_inject_dabt(ctx, hdfar);
		return TRAP_HANDLED;
	}

	if (is_write) {
		/* Load the value to write from the src register */
		access_cell_reg(ctx, srt, &mmio.value, true);
		if (sse)
			mmio.value = sign_extend(mmio.value, 8 * size);
	} else {
		mmio.value = 0;
	}
	mmio.is_write = is_write;
	mmio.size = size;

	mmio_result = mmio_handle_access(&mmio);
	if (mmio_result == MMIO_ERROR)
		return TRAP_FORBIDDEN;
	if (mmio_result == MMIO_UNHANDLED) {
		ret = irqchip_mmio_access(&mmio);

		if (ret == TRAP_FORBIDDEN)
			return TRAP_FORBIDDEN;
		if (ret == TRAP_UNHANDLED)
			goto error_unhandled;
	}

	/* Put the read value into the dest register */
	if (!is_write) {
		if (sse)
			mmio.value = sign_extend(mmio.value, 8 * size);
		access_cell_reg(ctx, srt, &mmio.value, false);
	}

	arch_skip_instruction(ctx);
	return TRAP_HANDLED;

error_unhandled:
	panic_printk("Unhandled data %s at 0x%x(%d)\n",
		(is_write ? "write" : "read"), mmio.address, size);

	return TRAP_UNHANDLED;
}
