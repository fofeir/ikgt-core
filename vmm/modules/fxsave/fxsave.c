/*******************************************************************************
* Copyright (c) 2017 Intel Corporation
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
#include "guest.h"
#include "gcpu.h"
#include "vmm_util.h"
#include "host_cpu.h"
#include "dbg.h"
#include "event.h"
#include "vmm_asm.h"
#include "vmm_arch.h"
#include "vmx_cap.h"

#include "modules/fxsave.h"

typedef struct fxsave_info {
	guest_cpu_handle_t gcpu;
	void *fxsave_area;
	struct fxsave_info *next;
} fxsave_info_t;

#define FXSAVE_AREA_SIZE   512

static fxsave_info_t *g_fxsave;
static vmm_lock_t fxsave_lock;

static boolean_t fxsave_is_supported()
{
	cpuid_params_t cpuid_params;

	cpuid_params.eax = 1;
	asm_cpuid(&cpuid_params);
	if ((cpuid_params.edx & CPUID_EDX_FXSAVE) == 0) {
		return FALSE;
	}
	return TRUE;
}

static fxsave_info_t *fxsave_lookup(guest_cpu_handle_t gcpu)
{
	fxsave_info_t *p_fxsave;

	lock_acquire_read(&fxsave_lock);
	p_fxsave = g_fxsave;
	while (p_fxsave) {
		if (p_fxsave->gcpu == gcpu)
			break;
		p_fxsave = p_fxsave->next;
	}

	lock_release(&fxsave_lock);
	return p_fxsave;
}

static void fxsave_swap_in(guest_cpu_handle_t gcpu, UNUSED void *pv)
{
	fxsave_info_t *fxsave;

	fxsave = fxsave_lookup(gcpu);
	if(fxsave == NULL)
	{
		/*for first swap in, register it to g_fxsave, but no need to
		 do fxrestore*/

		fxsave = (fxsave_info_t *)mem_alloc(sizeof(fxsave_info_t));

		/*fxsave area needs to be 16 byte aligned while the return from
		 mem_alloc() is always 16 byte aligned*/
		fxsave->fxsave_area = mem_alloc(FXSAVE_AREA_SIZE);
		fxsave->gcpu = gcpu;
		lock_acquire_write(&fxsave_lock);
		fxsave->next = g_fxsave;
		g_fxsave = fxsave;
		lock_release(&fxsave_lock);
	}else{
		asm_fxrstor(fxsave->fxsave_area);
	}
}

static void fxsave_swap_out(guest_cpu_handle_t gcpu, UNUSED void *pv)
{
	fxsave_info_t *fxsave;

	fxsave = fxsave_lookup(gcpu);

	/*it must be swapped in before, so the fxsave
	 for this gcpu must exist*/
	D(VMM_ASSERT(fxsave);)
	D(VMM_ASSERT(fxsave->fxsave_area);)

	asm_fxsave(fxsave->fxsave_area);
}

void fxsave_enable(void)
{
	asm_set_cr4(asm_get_cr4() | CR4_OSFXSR);
}

/*fxsave will isolate FPU/MMX/SSE registers*/
void fxsave_isolation_init(void)
{
	D(VMM_ASSERT_EX(fxsave_is_supported(),
		"fxsave is not supported\n");)
	VMM_ASSERT_EX((get_cr4_cap(NULL) & CR4_OSFXSR),
		"OSFXSR in cr4 is not supported by vmx cr4_may1\n");

	lock_init(&fxsave_lock, "fxsave_lock");
	event_register(EVENT_GCPU_SWAPIN, fxsave_swap_in);
	event_register(EVENT_GCPU_SWAPOUT, fxsave_swap_out);
}
