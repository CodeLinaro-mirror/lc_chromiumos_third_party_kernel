// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 Citrix Systems
 *
 * Author: Stefano Stabellini <stefano.stabellini@eu.citrix.com>
 */

#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/types.h>
#include <asm/paravirt.h>

struct static_key paravirt_steal_enabled;
struct static_key paravirt_steal_rq_enabled;

struct paravirt_patch_template pv_ops = {
	.state.vcpu_is_preempted = native_vcpu_is_preempted,
	.state.vcpu_preempt_count_update = native_vcpu_preempt_count_update,
};
EXPORT_SYMBOL_GPL(pv_ops);
