/*******************************************************************************
* Copyright (c) 2015 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
#include "vmm_asm.h"
#include "vmm_base.h"

#include "lib/lapic_ipi.h"

typedef enum {
	LAPIC_ID_REG = 0x2,
	LAPIC_INTR_CMD_REG = 0x30, /* 64-bits in x2APIC*/
	LAPIC_INTR_CMD_HI_REG = 0x31 /* not available in x2APIC*/
} lapic_reg_id_t;

#define MSR_APIC_BASE 0x1B
#define LAPIC_ENABLED  (1ULL << 11)
#define LAPIC_X2_ENABLED  (1ULL << 10)
#define LAPIC_BASE_ADDR(base_msr) ((base_msr) & (~PAGE_4K_MASK))

static uint32_t lapic_x1_read_reg(lapic_reg_id_t reg_id)
{
	uint64_t apic_base_msr = asm_rdmsr(MSR_APIC_BASE);
	//VMM_ASSERT(reg_id <= LAPIC_LAST_REG); // asserted in caller
	//assume hpa == hva.
	return *(volatile uint32_t*)(LAPIC_BASE_ADDR(apic_base_msr) + (reg_id<<4));
}

static void lapic_x1_write_reg(lapic_reg_id_t reg_id, uint32_t data)
{
	uint64_t apic_base_msr = asm_rdmsr(MSR_APIC_BASE);
	//VMM_ASSERT(reg_id <= LAPIC_LAST_REG); // asserted in caller
	//assume hpa == hva.
	*(volatile uint32_t*)(LAPIC_BASE_ADDR(apic_base_msr) + (reg_id<<4)) = data;
}

//deliver status bit 12. 0 idle, 1 send pending.
#define APIC_DS_BIT (1<<12)

//caller must make sure xAPIC mode.
static void lapic_x1_wait_for_ipi(void)
{
	uint32_t icr_low;

	while (1) {
		icr_low = lapic_x1_read_reg(LAPIC_INTR_CMD_REG);
		if ((icr_low & APIC_DS_BIT) == 0)
			return;
	}
}

#define MSR_X2APIC_BASE 0x800

static uint64_t lapic_x2_read_reg(lapic_reg_id_t reg_id)
{
	//VMM_ASSERT(reg_id <= LAPIC_LAST_REG); // asserted in caller
	return asm_rdmsr(MSR_X2APIC_BASE + reg_id);
}

static void lapic_x2_write_reg(lapic_reg_id_t reg_id, uint64_t data)
{
	//VMM_ASSERT(reg_id <= LAPIC_LAST_REG); // asserted in caller
	asm_wrmsr(MSR_X2APIC_BASE + reg_id, data);
}

#define APIC_DM_FIXED 0x000
#define APIC_DM_NMI 0x400
#define APIC_DM_INIT 0X500
#define APIC_DM_STARTUP 0x600
#define APIC_LEVEL_ASSERT 0x4000
#define APIC_DEST_NOSHORT 0x00000
#define APIC_DEST_EXCLUDE 0xC0000

/* When adding new APIs, please check IA32 spec -> Local APIC chapter
** -> ICR section, to learn the valid combination of destination
** shorthand, deliver mode, and trigger mode for Pentium 4 Processor */
static boolean_t lapic_send_ipi_excluding_self(uint32_t delivery_mode, uint32_t vector)
{
	uint32_t icr_low = APIC_DEST_EXCLUDE|APIC_LEVEL_ASSERT|delivery_mode|vector;
	uint64_t apic_base_msr = asm_rdmsr(MSR_APIC_BASE);

	if (!(apic_base_msr & LAPIC_ENABLED)) {
		return FALSE;
	}

	if (apic_base_msr & LAPIC_X2_ENABLED) {
		//x2APIC
		lapic_x2_write_reg(LAPIC_INTR_CMD_REG, (uint64_t)icr_low);
	}else {
		//xAPIC
		//need wait in x1 APIC only.
		lapic_x1_wait_for_ipi();
		lapic_x1_write_reg(LAPIC_INTR_CMD_REG, icr_low);
	}

	return TRUE;
}

static boolean_t lapic_send_ipi_to_cpu(uint32_t lapic_id, uint32_t delivery_mode, uint32_t vector)
{
	uint32_t icr_hi;
	uint32_t icr_low = APIC_DEST_NOSHORT|APIC_LEVEL_ASSERT|delivery_mode|vector;
	uint64_t apic_base_msr = asm_rdmsr(MSR_APIC_BASE);

	if (!(apic_base_msr & LAPIC_ENABLED)) {
		return FALSE;
	}

	if (apic_base_msr & LAPIC_X2_ENABLED) {
		//x2APIC
		lapic_x2_write_reg(LAPIC_INTR_CMD_REG, MAKE64(lapic_id, icr_low));
	}else {
		//xAPIC
		icr_hi = lapic_x1_read_reg(LAPIC_INTR_CMD_HI_REG); //save guest ICR_HI
		lapic_x1_write_reg(LAPIC_INTR_CMD_HI_REG, lapic_id << 24);
		//need wait in x1 APIC only.
		lapic_x1_wait_for_ipi();
		lapic_x1_write_reg(LAPIC_INTR_CMD_REG, icr_low);
		lapic_x1_write_reg(LAPIC_INTR_CMD_HI_REG, icr_hi); //restore guest ICR_HI
	}

	return TRUE;
}

boolean_t lapic_get_id(uint32_t *p_lapic_id)
{
	uint64_t apic_base_msr = asm_rdmsr(MSR_APIC_BASE);

	if (!(apic_base_msr & LAPIC_ENABLED)) {
		return FALSE;
	}

	if (apic_base_msr & LAPIC_X2_ENABLED) {
		//x2APIC
		*p_lapic_id = (uint32_t)lapic_x2_read_reg(LAPIC_ID_REG);
	}else {
		//xAPIC
		*p_lapic_id = lapic_x1_read_reg(LAPIC_ID_REG) >> 24;
	}

	return TRUE;
}

boolean_t broadcast_nmi(void)
{
	return lapic_send_ipi_excluding_self(APIC_DM_NMI, 0);
}

boolean_t broadcast_init(void)
{
	return lapic_send_ipi_excluding_self(APIC_DM_INIT, 0);
}

boolean_t broadcast_startup(uint32_t vector)
{
	return lapic_send_ipi_excluding_self(APIC_DM_STARTUP, vector);
}

boolean_t send_nmi(uint32_t lapic_id)
{
	return lapic_send_ipi_to_cpu(lapic_id, APIC_DM_NMI, 0);
}

boolean_t send_startup(uint32_t lapic_id, uint32_t vector)
{
	return lapic_send_ipi_to_cpu(lapic_id, APIC_DM_STARTUP, vector);
}