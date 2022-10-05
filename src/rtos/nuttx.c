// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright 2016,2017 Sony Video & Sound Products Inc.                  *
 *   Masatoshi Tateishi - Masatoshi.Tateishi@jp.sony.com                   *
 *   Masayuki Ishikawa - Masayuki.Ishikawa@jp.sony.com                     *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/jtag.h>
#include "target/target.h"
#include "target/target_type.h"
#include "target/armv7m.h"
#include "target/cortex_m.h"
#include "rtos.h"
#include "helper/log.h"
#include "helper/types.h"
#include "server/gdb_server.h"
#include "target/register.h"
#include "rtos_nuttx_stackings.h"

#define NAME_SIZE		32
#define EXTRAINFO_SIZE	256

/* Only 32-bit CPUs are supported by the current implementation.  Supporting
 * other CPUs will require reading this information from the target and
 * adapting the code accordignly.
 */
#define PTR_WIDTH 4

struct nuttx_params {
	const char *target_name;
	const struct rtos_register_stacking *(*select_stackinfo)(struct target *target);
};

static const struct rtos_register_stacking *cortexm_select_stackinfo(struct target *target);
static const struct rtos_register_stacking *riscv_select_stackinfo(struct target *target);

struct tcbinfo {
	uint8_t pid_off[2];
	uint8_t state_off[2];
	uint8_t pri_off[2];
	uint8_t name_off[2];
	uint8_t regs_off[2];
	uint8_t basic_num[2];
	uint8_t total_num[2];
} __attribute__ ((packed));

struct symbols {
	const char *name;
	bool optional;
};

/* Used to index the list of retrieved symbols. See nuttx_symbol_list for the order. */
enum nuttx_symbol_vals {
	NX_SYM_READYTORUN = 0,
	NX_SYM_PIDHASH,
	NX_SYM_NPIDHASH,
	NX_SYM_TCB_INFO,
};

/* See nuttx/sched/nx_start.c */
static const struct symbols nuttx_symbol_list[] = {
	{ "g_readytorun", false },
	{ "g_pidhash", false },
	{ "g_npidhash", false },
	{ "g_tcbinfo", false },
	{ NULL, false }
};

static char *task_state_str[] = {
	"INVALID",
	"PENDING",
	"READYTORUN",
	"RUNNING",
	"INACTIVE",
	"WAIT_SEM",
	"WAIT_SIG",
	"WAIT_MQNOTEMPTY",
	"WAIT_MQNOTFULL",
	"WAIT_PAGEFILL",
	"STOPPED",
};

static const struct nuttx_params nuttx_params_list[] = {
	{
		.target_name = "cortex_m",
		.select_stackinfo = cortexm_select_stackinfo,
	},
	{
		.target_name = "hla_target",
		.select_stackinfo = cortexm_select_stackinfo,
	},
	{
		.target_name = "esp32c3",
		.select_stackinfo = riscv_select_stackinfo,
	},
};

static bool cortexm_hasfpu(struct target *target)
{
	uint32_t cpacr;
	struct armv7m_common *armv7m_target = target_to_armv7m(target);

	if (!is_armv7m(armv7m_target) || armv7m_target->fp_feature != FPV4_SP)
		return false;

	int retval = target_read_u32(target, FPU_CPACR, &cpacr);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not read CPACR register to check FPU state");
		return false;
	}

	return cpacr & 0x00F00000;
}

static const struct rtos_register_stacking *cortexm_select_stackinfo(struct target *target)
{
	return cortexm_hasfpu(target) ? &nuttx_stacking_cortex_m_fpu : &nuttx_stacking_cortex_m;
}

static const struct rtos_register_stacking *riscv_select_stackinfo(struct target *target)
{
	return &nuttx_riscv_stacking;
}

static bool nuttx_detect_rtos(struct target *target)
{
	if ((target->rtos->symbols) &&
		(target->rtos->symbols[NX_SYM_READYTORUN].address != 0) &&
		(target->rtos->symbols[NX_SYM_PIDHASH].address != 0))
		return true;
	return false;
}

static int nuttx_create(struct target *target)
{
	const struct nuttx_params *param;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(nuttx_params_list); i++) {
		param = &nuttx_params_list[i];
		if (strcmp(target_type_name(target), param->target_name) == 0) {
			LOG_INFO("Detected target \"%s\"", param->target_name);
			break;
		}
	}

	if (i >= ARRAY_SIZE(nuttx_params_list)) {
		LOG_ERROR("Could not find \"%s\" target in NuttX compatibility list",
			target_type_name(target));
		return JIM_ERR;
	}

	/* We found a target in our list, copy its reference. */
	target->rtos->rtos_specific_params = (void *)param;

	return JIM_OK;
}

static int nuttx_update_threads(struct rtos *rtos)
{
	struct tcbinfo tcbinfo;
	uint32_t thread_count, pidhashaddr, npidhash, tcbaddr;
	uint16_t pid;
	uint8_t state;

	if (!rtos->symbols) {
		LOG_ERROR("No symbols for NuttX");
		return ERROR_FAIL;
	}

	/* Free previous thread details */
	rtos_free_threadlist(rtos);

	/* NuttX provides a hash table that keeps track of all the TCBs.
	 * We first read its size from g_npidhash and its address from g_pidhash.
	 * Its content is then read from these values.
	 */
	int ret = target_read_u32(rtos->target, rtos->symbols[NX_SYM_NPIDHASH].address, &npidhash);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read g_npidhash: ret = %d", ret);
		return ERROR_FAIL;
	}

	LOG_DEBUG("Hash table size (g_npidhash) = %" PRId32, npidhash);

	ret = target_read_u32(rtos->target, rtos->symbols[NX_SYM_PIDHASH].address, &pidhashaddr);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read g_pidhash address: ret = %d", ret);
		return ERROR_FAIL;
	}

	LOG_DEBUG("Hash table address (g_pidhash) = %" PRIx32, pidhashaddr);

	uint8_t *pidhash = malloc(npidhash * PTR_WIDTH);
	if (!pidhash) {
		LOG_ERROR("Failed to allocate pidhash");
		return ERROR_FAIL;
	}

	ret = target_read_buffer(rtos->target, pidhashaddr, PTR_WIDTH * npidhash, pidhash);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read tcbhash: ret = %d", ret);
		goto errout;
	}

	/* NuttX provides a struct that contains TCB offsets for required members.
	 * Read its content from g_tcbinfo.
	 */
	ret = target_read_buffer(rtos->target, rtos->symbols[NX_SYM_TCB_INFO].address,
		sizeof(tcbinfo), (uint8_t *)&tcbinfo);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read tcbinfo: ret = %d", ret);
		goto errout;
	}

	/* The head of the g_readytorun list is the currently running task.
	 * Reading in a temporary variable first to avoid endianness issues,
	 * rtos->current_thread is int64_t. */
	uint32_t current_thread;
	ret = target_read_u32(rtos->target, rtos->symbols[NX_SYM_READYTORUN].address, &current_thread);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read g_readytorun: ret = %d", ret);
		goto errout;
	}
	rtos->current_thread = current_thread;

	thread_count = 0;

	for (unsigned int i = 0; i < npidhash; i++) {
		tcbaddr = le_to_h_u32(&pidhash[i * sizeof(uint32_t)]);

		if (tcbaddr) {
			struct thread_detail *thread;

			ret = target_read_u16(rtos->target, tcbaddr + le_to_h_u16(tcbinfo.pid_off), &pid);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to read PID of TCB@0x%x from pidhash[%d]: ret = %d",
					tcbaddr, i, ret);
				goto errout;
			}

			ret = target_read_u8(rtos->target, tcbaddr + le_to_h_u16(tcbinfo.state_off), &state);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to read state of TCB@0x%x from pidhash[%d]: ret = %d",
					tcbaddr, i, ret);
				goto errout;
			}

			thread_count++;

			rtos->thread_details = realloc(rtos->thread_details,
				sizeof(struct thread_detail) * thread_count);
			if (!rtos->thread_details) {
				ret = ERROR_FAIL;
				goto errout;
			}

			thread = &rtos->thread_details[thread_count - 1];
			thread->threadid = tcbaddr;
			thread->exists = true;

			thread->extra_info_str = NULL;
			if (state < ARRAY_SIZE(task_state_str)) {
				thread->extra_info_str = malloc(EXTRAINFO_SIZE);
				if (!thread->extra_info_str) {
					ret = ERROR_FAIL;
					goto errout;
				}
				snprintf(thread->extra_info_str, EXTRAINFO_SIZE, "pid:%d, %s",
					pid,
					task_state_str[state]);
			}

			if (le_to_h_u16(tcbinfo.name_off)) {
				thread->thread_name_str = calloc(NAME_SIZE + 1, sizeof(char));
				if (!thread->thread_name_str) {
					ret = ERROR_FAIL;
					goto errout;
				}
				ret = target_read_buffer(rtos->target, tcbaddr + le_to_h_u16(tcbinfo.name_off),
					sizeof(char) * NAME_SIZE, (uint8_t *)thread->thread_name_str);
				if (ret != ERROR_OK) {
					LOG_ERROR("Failed to read thread's name: ret = %d", ret);
					goto errout;
				}
			} else {
				thread->thread_name_str = strdup("None");
			}
		}
	}

	ret = ERROR_OK;
	rtos->thread_count = thread_count;
errout:
	free(pidhash);
	return ret;
}

static int nuttx_getreg_current_thread(struct rtos *rtos,
	struct rtos_reg **reg_list, int *num_regs)
{
	struct reg **gdb_reg_list;

	/* Registers for currently running thread are not on task's stack and
	 * should be retrieved from reg caches via target_get_gdb_reg_list */
	int ret = target_get_gdb_reg_list(rtos->target, &gdb_reg_list, num_regs,
		REG_CLASS_GENERAL);
	if (ret != ERROR_OK) {
		LOG_ERROR("target_get_gdb_reg_list failed %d", ret);
		return ret;
	}

	*reg_list = calloc(*num_regs, sizeof(struct rtos_reg));
	if (!(*reg_list)) {
		LOG_ERROR("Failed to alloc memory for %d", *num_regs);
		free(gdb_reg_list);
		return ERROR_FAIL;
	}

	for (int i = 0; i < *num_regs; i++) {
		(*reg_list)[i].number = gdb_reg_list[i]->number;
		(*reg_list)[i].size = gdb_reg_list[i]->size;
		memcpy((*reg_list)[i].value, gdb_reg_list[i]->value, ((*reg_list)[i].size + 7) / 8);
	}

	free(gdb_reg_list);

	return ERROR_OK;
}

static int nuttx_getregs_fromstack(struct rtos *rtos, int64_t thread_id,
	struct rtos_reg **reg_list, int *num_regs)
{
	const struct rtos_register_stacking *stacking;
	uint16_t xcpreg_off;
	uint32_t regsaddr;
	const struct nuttx_params *priv = (const struct nuttx_params *)rtos->rtos_specific_params;

	if (priv->select_stackinfo) {
		stacking = priv->select_stackinfo(rtos->target);
	} else {
		LOG_ERROR("Can't find a way to select stacking info");
		return ERROR_FAIL;
	}

	int ret = target_read_u16(rtos->target,
		rtos->symbols[NX_SYM_TCB_INFO].address + offsetof(struct tcbinfo, regs_off),
		&xcpreg_off);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read registers' offset: ret = %d", ret);
		return ERROR_FAIL;
	}

	ret = target_read_u32(rtos->target, thread_id + xcpreg_off, &regsaddr);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read registers' address: ret = %d", ret);
		return ERROR_FAIL;
	}

	return rtos_generic_stack_read(rtos->target, stacking, regsaddr, reg_list, num_regs);
}

static int nuttx_get_thread_reg_list(struct rtos *rtos, int64_t thread_id,
	struct rtos_reg **reg_list, int *num_regs)
{
	if (!rtos)
		return ERROR_FAIL;

	if (thread_id == rtos->current_thread)
		return nuttx_getreg_current_thread(rtos, reg_list, num_regs);
	return nuttx_getregs_fromstack(rtos, thread_id, reg_list, num_regs);
}

static int nuttx_get_symbol_list_to_lookup(struct symbol_table_elem *symbol_list[])
{
	*symbol_list = (struct symbol_table_elem *)calloc(1,
		sizeof(struct symbol_table_elem) * ARRAY_SIZE(nuttx_symbol_list));

	for (unsigned int i = 0; i < ARRAY_SIZE(nuttx_symbol_list); i++) {
		(*symbol_list)[i].symbol_name = nuttx_symbol_list[i].name;
		(*symbol_list)[i].optional = nuttx_symbol_list[i].optional;
	}

	return ERROR_OK;
}

struct rtos_type nuttx_rtos = {
	.name = "NuttX",
	.detect_rtos = nuttx_detect_rtos,
	.create = nuttx_create,
	.update_threads = nuttx_update_threads,
	.get_thread_reg_list = nuttx_get_thread_reg_list,
	.get_symbol_list_to_lookup = nuttx_get_symbol_list_to_lookup,
};
