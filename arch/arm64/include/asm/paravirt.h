/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_PARAVIRT_H
#define _ASM_ARM64_PARAVIRT_H

#ifdef CONFIG_PARAVIRT
struct static_key;
extern struct static_key paravirt_steal_enabled;
extern struct static_key paravirt_steal_rq_enabled;

struct pvstate_vcpu_info {
	bool	preempted;
	// padding
	int	preempt_count;
	u8	reserved[56];
};

struct pv_state_ops {
	bool (*vcpu_is_preempted)(int cpu);
	void (*vcpu_preempt_count_update)(int val);
};

struct pv_time_ops {
	unsigned long long (*steal_clock)(int cpu);
};

struct paravirt_patch_template {
	struct pv_time_ops time;
	struct pv_state_ops state;
};

extern struct paravirt_patch_template pv_ops;

static inline u64 paravirt_steal_clock(int cpu)
{
	return pv_ops.time.steal_clock(cpu);
}

bool native_vcpu_is_preempted(int cpu);
bool paravirt_vcpu_is_preempted(int cpu);

void native_vcpu_preempt_count_update(int val);
void paravirt_vcpu_preempt_count_update(int val);

int pv_state_init(void);

#else

#define pv_state_init() do {} while (0)

#endif /* CONFIG_PARAVIRT */

#endif
