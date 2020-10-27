// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "arm-pvstate: " fmt

#include <linux/arm-smccc.h>
#include <linux/cpuhotplug.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/jump_label.h>
#include <linux/printk.h>
#include <linux/psci.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <asm/paravirt.h>
#include <asm/smp_plat.h>

static DEFINE_PER_CPU(struct pvstate_vcpu_info, vcpus_states);

bool native_vcpu_is_preempted(int cpu)
{
	return false;
}

static bool pv_vcpu_is_preempted(int cpu)
{
	struct pvstate_vcpu_info *st;

	st = &per_cpu(vcpus_states, cpu);
	return READ_ONCE(st->preempted);
}

bool paravirt_vcpu_is_preempted(int cpu)
{
	return pv_ops.state.vcpu_is_preempted(cpu);
}

static bool has_pvstate(void)
{
	struct arm_smccc_res res;

	/* To detect the presence of PV time support we require SMCCC 1.1+ */
	if (arm_smccc_1_1_get_conduit() == SMCCC_CONDUIT_NONE)
		return false;

	arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
			     ARM_SMCCC_HV_PV_STATE_FEATURES,
			     &res);

	if (res.a0 != SMCCC_RET_SUCCESS)
		return false;
	return true;
}

static int __pvstate_cpu_hook(unsigned int cpu, int event)
{
	struct arm_smccc_res res;
	struct pvstate_vcpu_info *st;

	st = &per_cpu(vcpus_states, cpu);
	arm_smccc_1_1_invoke(event, virt_to_phys(st), &res);
	if (res.a0 != SMCCC_RET_SUCCESS)
		return -EINVAL;
	return 0;
}

static int pvstate_cpu_init(unsigned int cpu)
{
	int ret = __pvstate_cpu_hook(cpu, ARM_SMCCC_HV_PV_STATE_INIT);

	if (ret)
		pr_warn("Unable to ARM_SMCCC_HV_PV_STATE_INIT\n");
	return ret;
}

static int pvstate_cpu_release(unsigned int cpu)
{
	int ret = __pvstate_cpu_hook(cpu, ARM_SMCCC_HV_PV_STATE_RELEASE);

	if (ret)
		pr_warn("Unable to ARM_SMCCC_HV_PV_STATE_RELEASE\n");
	return ret;
}

static int pvstate_register_hooks(void)
{
	int ret;

	ret = cpuhp_setup_state(CPUHP_AP_KVM_STARTING,
			"hypervisor/arm/pvstate:starting",
			pvstate_cpu_init,
			pvstate_cpu_release);
	if (ret < 0)
		pr_warn("Failed to register CPU hooks\n");
	return ret;
}

static int __pvstate_init(void)
{
	return pvstate_register_hooks();
}

int __init pv_state_init(void)
{
	int ret;

	if (!has_pvstate())
		return 0;

	ret = __pvstate_init();
	if (ret)
		return ret;

	pv_ops.state.vcpu_is_preempted = pv_vcpu_is_preempted;
	return 0;
}
