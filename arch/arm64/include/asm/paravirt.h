/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_PARAVIRT_H
#define _ASM_ARM64_PARAVIRT_H

#ifdef CONFIG_PARAVIRT
struct static_key;
extern struct static_key paravirt_steal_enabled;
extern struct static_key paravirt_steal_rq_enabled;

struct pvstate_vcpu_info {
	bool	preempted;
	u8	reserved[63];
};

struct pv_state_ops {
	bool (*vcpu_is_preempted)(int cpu);
};
extern struct pv_state_ops pv_state_ops;

struct pv_time_ops {
	unsigned long long (*steal_clock)(int cpu);
};
extern struct pv_time_ops pv_time_ops;

static inline u64 paravirt_steal_clock(int cpu)
{
	return pv_time_ops.steal_clock(cpu);
}
#endif

#endif
