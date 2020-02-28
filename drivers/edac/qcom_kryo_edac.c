// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/cpu_pm.h>
#include <linux/of_irq.h>
#include <linux/smp.h>

#include <asm/cputype.h>
#include <asm/sysreg.h>

#include "edac_device.h"
#include "edac_mc.h"

#define DRV_NAME		"qcom_kryo_edac"

/*
 * ARM Cortex-A55, Cortex-A75, Cortex-A76 TRM Chapter B3.3
 * ARM DSU TRM Chapter B2.3
 * CFI = Corrected Fault Handling interrupt, FI = Fault handling interrupt
 * UI = Uncorrected error recovery interrupt, ED = Error Detection
 */
#define KRYO_ERRXCTLR_ED	BIT(0)
#define KRYO_ERRXCTLR_UI	BIT(2)
#define KRYO_ERRXCTLR_FI	BIT(3)
#define KRYO_ERRXCTLR_CFI	BIT(8)
#define KRYO_ERRXCTLR_ENABLE	(KRYO_ERRXCTLR_CFI | KRYO_ERRXCTLR_FI | \
				 KRYO_ERRXCTLR_UI | KRYO_ERRXCTLR_ED)

/*
 * ARM Cortex-A55, Cortex-A75, Cortex-A76 TRM Chapter B3.4
 * ARM DSU TRM Chapter B2.4
 */
#define KRYO_ERRXFR_ED		GENMASK(1, 0)
#define KRYO_ERRXFR_DE		GENMASK(3, 2)
#define KRYO_ERRXFR_UI		GENMASK(5, 4)
#define KRYO_ERRXFR_FI		GENMASK(7, 6)
#define KRYO_ERRXFR_UE		GENMASK(9, 8)
#define KRYO_ERRXFR_CFI		GENMASK(11, 10)
#define KRYO_ERRXFR_CEC		GENMASK(14, 12)
#define KRYO_ERRXFR_RP		BIT(15)
#define KRYO_ERRXFR_SUPPORTED	(KRYO_ERRXFR_ED | KRYO_ERRXFR_DE | \
				 KRYO_ERRXFR_UI | KRYO_ERRXFR_FI | \
				 KRYO_ERRXFR_UE | KRYO_ERRXFR_CFI | \
				 KRYO_ERRXFR_CEC | KRYO_ERRXFR_RP)

/*
 * ARM Cortex-A55, Cortex-A75, Cortex-A76 TRM Chapter B3.5
 * ARM DSU TRM Chapter B2.5
 */
#define KRYO_ERRXMISC0_CECR	GENMASK_ULL(38, 32)
#define KRYO_ERRXMISC0_CECO	GENMASK_ULL(46, 40)

/* ARM Cortex-A76 TRM Chapter B3.5 */
#define KRYO_ERRXMISC0_UNIT	GENMASK(3, 0)
#define KRYO_ERRXMISC0_LVL	GENMASK(3, 1)

/* ARM Cortex-A76 TRM Chapter B3.10 has SERR bitfields 4:0
 * but Cortex-A55, Cortex-A75 and DSU TRM has SERR bitfields 7:0.
 * Since max error record is 21, we can use bitfields 4:0 for
 * Kryo{3,4}XX CPUs.
 */
#define KRYO_ERRXSTATUS_SERR	GENMASK(4, 0)
#define KRYO_ERRXSTATUS_DE	BIT(23)
#define KRYO_ERRXSTATUS_CE	GENMASK(25, 24)
#define KRYO_ERRXSTATUS_MV	BIT(26)
#define KRYO_ERRXSTATUS_UE	BIT(29)
#define KRYO_ERRXSTATUS_VALID	BIT(30)

/* ARM Cortex-A76 TRM Chapter B3.5
 * IC = Instruction Cache, DC = Data Cache
 */
#define KRYO_L1_UNIT_IC		0x1
#define KRYO_L2_UNIT_TLB	0x2
#define KRYO_L1_UNIT_DC		0x4
#define KRYO_L2_UNIT		0x8

/*
 * ARM Cortex-A55 TRM Chapter B2.36
 * ARM Cortex-A75, Cortex-A76 TRM Chapter B2.37
 */
#define KRYO_ERR_RECORD_L1_L2	0x0
#define KRYO_ERR_RECORD_L3	0x1

/* ARM DSU TRM Chapter B2.10 */
#define BUS_ERROR		0x12

/* QCOM Kryo CPU part numbers */
#define KRYO3XX_GOLD		0x802
#define KRYO4XX_GOLD		0x804
#define KRYO4XX_SILVER_V1	0x803
#define KRYO4XX_SILVER_V2	0x805

#define KRYO_EDAC_MSG_MAX	256

static int poll_msec = 1000;
module_param(poll_msec, int, 0444);

enum {
	KRYO_L1 = 0,
	KRYO_L2,
	KRYO_L3,
};

/* CE = Corrected Error, UE = Uncorrected Error, DE = Deferred Error */
enum {
	KRYO_L1_CE = 0,
	KRYO_L1_UE,
	KRYO_L1_DE,
	KRYO_L2_CE,
	KRYO_L2_UE,
	KRYO_L2_DE,
	KRYO_L3_CE,
	KRYO_L3_UE,
	KRYO_L3_DE,
};

struct error_record {
	u32 error_code;
	const char *error_msg;
};

struct error_type {
	void (*fn)(struct edac_device_ctl_info *edev_ctl,
		   int inst_nr, int block_nr, const char *msg);
	const char *msg;
};

/*
 * ARM Cortex-A55, Cortex-A75, Cortex-A76 TRM Chapter B3.10
 * ARM DSU TRM Chapter B2.10
 */
static const struct error_record serror_record[] = {
	{ 0x1,	"Errors due to fault injection"		},
	{ 0x2,	"ECC error from internal data buffer"	},
	{ 0x6,	"ECC error on cache data RAM"		},
	{ 0x7,	"ECC error on cache tag or dirty RAM"	},
	{ 0x8,	"Parity error on TLB data RAM"		},
	{ 0x9,	"Parity error on TLB tag RAM"		},
	{ 0x12,	"Error response for a cache copyback"	},
	{ 0x15,	"Deferred error not supported"		},
};

static const struct error_type err_type[] = {
	{ edac_device_handle_ce, "Kryo L1 Corrected Error"	},
	{ edac_device_handle_ue, "Kryo L1 Uncorrected Error"	},
	{ edac_device_handle_ue, "Kryo L1 Deferred Error"	},
	{ edac_device_handle_ce, "Kryo L2 Corrected Error"	},
	{ edac_device_handle_ue, "Kryo L2 Uncorrected Error"	},
	{ edac_device_handle_ue, "Kryo L2 Deferred Error"	},
	{ edac_device_handle_ce, "L3 Corrected Error"		},
	{ edac_device_handle_ue, "L3 Uncorrected Error"		},
	{ edac_device_handle_ue, "L3 Deferred Error"		},
};

static struct edac_device_ctl_info __percpu *edac_dev;
static struct edac_device_ctl_info *drv_edev_ctl;

static const char *get_error_msg(u64 errxstatus)
{
	const struct error_record *rec;
	u32 errxstatus_serr;

	errxstatus_serr = FIELD_GET(KRYO_ERRXSTATUS_SERR, errxstatus);

	for (rec = serror_record; rec->error_code; rec++) {
		if (errxstatus_serr == rec->error_code)
			return rec->error_msg;
	}

	return NULL;
}

static void dump_syndrome_reg(int error_type, int level,
			      u64 errxstatus, u64 errxmisc,
			      struct edac_device_ctl_info *edev_ctl)
{
	char msg[KRYO_EDAC_MSG_MAX];
	const char *error_msg;
	int cpu;

	cpu = raw_smp_processor_id();

	error_msg = get_error_msg(errxstatus);
	if (!error_msg)
		return;

	snprintf(msg, KRYO_EDAC_MSG_MAX,
		 "CPU%d: %s, ERRXSTATUS_EL1:%#llx ERRXMISC0_EL1:%#llx, %s",
		 cpu, err_type[error_type].msg, errxstatus, errxmisc,
		 error_msg);

	err_type[error_type].fn(edev_ctl, 0, level, msg);
}

static void kryo_check_err_type(u64 errxstatus, u64 errxmisc,
				struct edac_device_ctl_info *edev_ctl,
				int level)
{
	u32 errxstatus_ue, errxstatus_ce, errxstatus_de;

	errxstatus_ce = FIELD_GET(KRYO_ERRXSTATUS_CE, errxstatus);
	errxstatus_ue = FIELD_GET(KRYO_ERRXSTATUS_UE, errxstatus);
	errxstatus_de = FIELD_GET(KRYO_ERRXSTATUS_DE, errxstatus);

	switch (level) {
	case KRYO_L1:
		if (errxstatus_ce)
			dump_syndrome_reg(KRYO_L1_CE, level, errxstatus,
					  errxmisc, edev_ctl);
		else if (errxstatus_ue)
			dump_syndrome_reg(KRYO_L1_UE, level, errxstatus,
					  errxmisc, edev_ctl);
		else if (errxstatus_de)
			dump_syndrome_reg(KRYO_L1_DE, level, errxstatus,
					  errxmisc, edev_ctl);
		else
			edac_printk(KERN_ERR, DRV_NAME, "Unknown error\n");
		break;
	case KRYO_L2:
		if (errxstatus_ce)
			dump_syndrome_reg(KRYO_L2_CE, level, errxstatus,
					  errxmisc, edev_ctl);
		else if (errxstatus_ue)
			dump_syndrome_reg(KRYO_L2_UE, level, errxstatus,
					  errxmisc, edev_ctl);
		else if (errxstatus_de)
			dump_syndrome_reg(KRYO_L2_DE, level, errxstatus,
					  errxmisc, edev_ctl);
		else
			edac_printk(KERN_ERR, DRV_NAME, "Unknown error\n");
		break;
	case KRYO_L3:
		if (errxstatus_ce)
			dump_syndrome_reg(KRYO_L3_CE, level, errxstatus,
					  errxmisc, edev_ctl);
		else if (errxstatus_ue)
			dump_syndrome_reg(KRYO_L3_UE, level, errxstatus,
					  errxmisc, edev_ctl);
		else if (errxstatus_de)
			dump_syndrome_reg(KRYO_L3_DE, level, errxstatus,
					  errxmisc, edev_ctl);
		else
			edac_printk(KERN_ERR, DRV_NAME, "Unknown error\n");
		break;
	default:
		edac_printk(KERN_ERR, DRV_NAME, "Unknown level\n");
	}
}

static inline void kryo_clear_error(u64 errxstatus)
{
	write_sysreg_s(errxstatus, SYS_ERXSTATUS_EL1);
	isb();
}

static void kryo_parse_l1_l2_cache_error(u64 errxstatus, u64 errxmisc,
					 struct edac_device_ctl_info *edev_ctl,
					 int cpu)
{
	u32 part_num = read_cpuid_part_number();

	switch (part_num) {
	/* Kryo3XX gold CPU cores do not have a UNIT bitfield */
	case KRYO3XX_GOLD:
	case KRYO4XX_SILVER_V1:
	case KRYO4XX_SILVER_V2:
		switch (FIELD_GET(KRYO_ERRXMISC0_LVL, errxmisc)) {
		case KRYO_L1:
			kryo_check_err_type(errxstatus, errxmisc,
					    edev_ctl, KRYO_L1);
			break;
		case KRYO_L2:
			kryo_check_err_type(errxstatus, errxmisc,
					    edev_ctl, KRYO_L2);
			break;
		default:
			edac_printk(KERN_ERR, DRV_NAME,
				    "silver cpu:%d unknown error: %lu\n", cpu,
				    FIELD_GET(KRYO_ERRXMISC0_LVL, errxmisc));
		}
		break;
	/* Kryo4XX gold CPU cores have a UNIT bitfield to identify levels */
	case KRYO4XX_GOLD:
		switch (FIELD_GET(KRYO_ERRXMISC0_UNIT, errxmisc)) {
		case KRYO_L1_UNIT_DC:
		case KRYO_L1_UNIT_IC:
			kryo_check_err_type(errxstatus, errxmisc,
					    edev_ctl, KRYO_L1);
			break;
		case KRYO_L2_UNIT:
		case KRYO_L2_UNIT_TLB:
			kryo_check_err_type(errxstatus, errxmisc,
					    edev_ctl, KRYO_L2);
			break;
		default:
			edac_printk(KERN_ERR, DRV_NAME,
				    "gold cpu:%d unknown error: %lu\n", cpu,
				    FIELD_GET(KRYO_ERRXMISC0_UNIT, errxmisc));
		}
		break;
	default:
		edac_printk(KERN_ERR, DRV_NAME,
			    "Error in matching cpu%d with part num:%u\n",
			    cpu, part_num);
	}
}

static inline bool kryo_check_regs_valid(u64 errxstatus)
{
	/* Check if status and misc regs are valid */
	if (!(FIELD_GET(KRYO_ERRXSTATUS_VALID, errxstatus)) ||
	    !(FIELD_GET(KRYO_ERRXSTATUS_MV, errxstatus)))
		return false;

	return true;
}

static void kryo_check_l1_l2_ecc(void *info)
{
	struct edac_device_ctl_info *edev_ctl = info;
	u64 errxstatus;
	u64 errxmisc;
	int cpu;

	cpu = smp_processor_id();
	/* We know record 0 is L1 and L2 */
	write_sysreg_s(0, SYS_ERRSELR_EL1);
	isb();

	errxstatus = read_sysreg_s(SYS_ERXSTATUS_EL1);
	if (!kryo_check_regs_valid(errxstatus))
		return;

	errxmisc = read_sysreg_s(SYS_ERXMISC0_EL1);
	/* Check if L1/L2 error */
	if (!(FIELD_GET(KRYO_ERRXMISC0_LVL, errxmisc) == KRYO_L1) &&
	    !(FIELD_GET(KRYO_ERRXMISC0_LVL, errxmisc) == KRYO_L2))
		return;

	kryo_parse_l1_l2_cache_error(errxstatus, errxmisc, edev_ctl, cpu);
	kryo_clear_error(errxstatus);
}

static irqreturn_t kryo_l1_l2_handler(int irq, void *drvdata)
{
	struct edac_device_ctl_info *edev_ctl = *(void **)drvdata;

	kryo_check_l1_l2_ecc(edev_ctl);

	return IRQ_HANDLED;
}

static bool kryo_check_l3_bus_error(u64 errxstatus)
{
	if (FIELD_GET(KRYO_ERRXSTATUS_SERR, errxstatus) == BUS_ERROR) {
		edac_printk(KERN_ERR, DRV_NAME, "Bus Error\n");
		return true;
	}

	return false;
}

static void kryo_check_l3_scu_ecc(struct edac_device_ctl_info *edev_ctl)
{
	u64 errxstatus, errxmisc;

	/* We know record 1 is L3-SCU */
	write_sysreg_s(1, SYS_ERRSELR_EL1);
	isb();

	errxstatus = read_sysreg_s(SYS_ERXSTATUS_EL1);
	if (!kryo_check_regs_valid(errxstatus))
		return;

	errxmisc = read_sysreg_s(SYS_ERXMISC0_EL1);
	/* Check if L3/bus error */
	if (!(FIELD_GET(KRYO_ERRXMISC0_LVL, errxmisc) == KRYO_L3) ||
	    kryo_check_l3_bus_error(errxstatus))
		return;

	/* Check if Corrected/Uncorrected/Deferred error and dump regs */
	kryo_check_err_type(errxstatus, errxmisc, edev_ctl, KRYO_L3);
	kryo_clear_error(errxstatus);
}

static irqreturn_t kryo_l3_scu_handler(int irq, void *edev_ctl)
{
	kryo_check_l3_scu_ecc(edev_ctl);

	return IRQ_HANDLED;
}

static void kryo_l1_l2_irq_disable(void *drvdata)
{
	int irq = *(int *)drvdata;

	disable_percpu_irq(irq);
}

static void kryo_l1_l2_irq_enable(void *drvdata)
{
	int irq = *(int *)drvdata;

	enable_percpu_irq(irq, IRQ_TYPE_LEVEL_HIGH);
}

static int kryo_l1_l2_setup_irq(struct platform_device *pdev,
				struct edac_device_ctl_info *edev_ctl)
{
	int cpu, errirq, faultirq, ret;

	edac_dev = devm_alloc_percpu(&pdev->dev, *edac_dev);
	if (!edac_dev)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		preempt_disable();
		per_cpu(edac_dev, cpu) = edev_ctl;
		preempt_enable();
	}

	faultirq = platform_get_irq_byname(pdev, "l1-l2-faultirq");
	if (faultirq < 0) {
		ret = faultirq;
		goto out_fault;
	}

	ret = request_percpu_irq(faultirq, kryo_l1_l2_handler,
				 "kryo_l1_l2_ecc_faultirq",
				 &edac_dev);
	if (ret) {
		edac_printk(KERN_DEBUG, DRV_NAME,
			    "Failed to request l1-l2-faultirq %d\n", faultirq);
		goto out_fault;
	}

	on_each_cpu(kryo_l1_l2_irq_enable, &faultirq, 1);

out_fault:
	errirq = platform_get_irq_byname(pdev, "l1-l2-errirq");
	if (errirq < 0) {
		ret = errirq;
		goto out_err;
	}

	ret = request_percpu_irq(errirq, kryo_l1_l2_handler,
				 "kryo_l1_l2_ecc_errirq",
				 &edac_dev);
	if (ret) {
		edac_printk(KERN_DEBUG, DRV_NAME,
			    "Failed to request l1-l2-errirq %d\n", errirq);
		goto out_err;
	}

	on_each_cpu(kryo_l1_l2_irq_enable, &errirq, 1);

	return ret;

out_err:
	free_percpu_irq(faultirq, &edac_dev);

	return ret;
}

static int kryo_l3_setup_irq(struct platform_device *pdev, void *edev_ctl)
{
	int errirq, faultirq, ret;

	faultirq = platform_get_irq_byname(pdev, "l3-scu-faultirq");
	if (faultirq < 0) {
		ret = faultirq;
		goto out_fault;
	}

	ret = devm_request_irq(&pdev->dev, faultirq, kryo_l3_scu_handler,
			       IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
			       "kryo_l3_scu_ecc_faultirq", edev_ctl);
	if (ret) {
		edac_printk(KERN_DEBUG, DRV_NAME,
			    "Failed to request l3-scu-faultirq %d\n", faultirq);
	}

out_fault:
	errirq = platform_get_irq_byname(pdev, "l3-scu-errirq");
	if (errirq < 0) {
		ret = errirq;
		goto out_err;
	}

	ret = devm_request_irq(&pdev->dev, errirq, kryo_l3_scu_handler,
			       IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
			       "kryo_l3_scu_ecc_errirq", edev_ctl);
	if (ret) {
		edac_printk(KERN_DEBUG, DRV_NAME,
			    "Failed to request l3-scu-errirq %d\n", errirq);
	}

out_err:
	return ret;
}

static int qcom_kryo_edac_setup_irq(struct platform_device *pdev,
				    struct edac_device_ctl_info *edev_ctl)
{
	int ret;

	ret = kryo_l1_l2_setup_irq(pdev, edev_ctl);
	if (ret) {
		edac_printk(KERN_DEBUG, DRV_NAME,
			    "Failed to setup l1-l2 irq\n");
	}

	ret = kryo_l3_setup_irq(pdev, edev_ctl);
	if (ret) {
		edac_printk(KERN_DEBUG, DRV_NAME,
			    "Failed to setup l3-scu irq\n");
	}

	return ret;
}

static void kryo_poll_cache_error(struct edac_device_ctl_info *edev_ctl)
{
	if (!edev_ctl)
		edev_ctl = drv_edev_ctl;

	on_each_cpu(kryo_check_l1_l2_ecc, edev_ctl, 1);
	kryo_check_l3_scu_ecc(edev_ctl);
}

static inline void kryo_enable_err_record(void)
{
	write_sysreg_s(KRYO_ERRXCTLR_ENABLE, SYS_ERXCTLR_EL1);
	write_sysreg_s(KRYO_ERRXMISC0_CECR | KRYO_ERRXMISC0_CECO,
		       SYS_ERXMISC0_EL1);
	isb();
}

static inline void qcom_kryo_init_l3(void)
{
	if (!FIELD_GET(KRYO_ERRXFR_SUPPORTED, read_sysreg_s(SYS_ERXFR_EL1)))
		return;

	/* L3 is shared */
	write_sysreg_s(KRYO_ERR_RECORD_L3, SYS_ERRSELR_EL1);
	kryo_enable_err_record();
}

static void qcom_kryo_init_l1_l2(void *data)
{
	if (!FIELD_GET(KRYO_ERRXFR_SUPPORTED, read_sysreg_s(SYS_ERXFR_EL1)))
		return;

	/* L1 and L2 is per-cpu */
	write_sysreg_s(KRYO_ERR_RECORD_L1_L2, SYS_ERRSELR_EL1);
	kryo_enable_err_record();
}

static int kryo_edac_pm_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	switch (action) {
	case CPU_PM_EXIT:
		qcom_kryo_init_l1_l2(NULL);
		qcom_kryo_init_l3();
		kryo_check_l1_l2_ecc(drv_edev_ctl);
		kryo_check_l3_scu_ecc(drv_edev_ctl);
		break;
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static struct notifier_block kryo_edac_pm_nb = {
	.notifier_call = kryo_edac_pm_notify,
};

static inline void qcom_kryo_edac_setup(void)
{
	on_each_cpu(qcom_kryo_init_l1_l2, NULL, 1);
	qcom_kryo_init_l3();
}

static int qcom_kryo_edac_probe(struct platform_device *pdev)
{
	struct edac_device_ctl_info *edev_ctl;
	struct device *dev = &pdev->dev;
	int ret;

	qcom_kryo_edac_setup();

	edev_ctl = edac_device_alloc_ctl_info(0, DRV_NAME, 1, "L", 3, 1, NULL,
					      0, edac_device_alloc_index());
	if (!edev_ctl)
		return -ENOMEM;

	if (IS_ENABLED(CONFIG_EDAC_QCOM_KRYO_POLL)) {
		edev_ctl->poll_msec = poll_msec;
		edev_ctl->edac_check = kryo_poll_cache_error;
	}
	edev_ctl->dev = dev;
	edev_ctl->mod_name = DRV_NAME;
	edev_ctl->dev_name = dev_name(dev);
	edev_ctl->ctl_name = "qcom_kryo_cache";
	edev_ctl->panic_on_ue = 1;

	ret = edac_device_add_device(edev_ctl);
	if (ret)
		goto out_mem;

	platform_set_drvdata(pdev, edev_ctl);
	drv_edev_ctl = edev_ctl;

	ret = qcom_kryo_edac_setup_irq(pdev, edev_ctl);
	if (ret)
		goto out_dev;

	cpu_pm_register_notifier(&kryo_edac_pm_nb);

	return ret;

out_dev:
	edac_device_del_device(edev_ctl->dev);
out_mem:
	edac_device_free_ctl_info(edev_ctl);

	return ret;
}

static void qcom_kryo_edac_teardown(struct platform_device *pdev)
{
	int errirq, faultirq;

	faultirq = platform_get_irq_byname(pdev, "l1-l2-faultirq");
	on_each_cpu(kryo_l1_l2_irq_disable, &faultirq, 1);
	free_percpu_irq(faultirq, &edac_dev);

	errirq = platform_get_irq_byname(pdev, "l1-l2-errirq");
	on_each_cpu(kryo_l1_l2_irq_disable, &errirq, 1);
	free_percpu_irq(errirq, &edac_dev);

	cpu_pm_unregister_notifier(&kryo_edac_pm_nb);
}

static int qcom_kryo_edac_remove(struct platform_device *pdev)
{
	struct edac_device_ctl_info *edev_ctl = platform_get_drvdata(pdev);

	qcom_kryo_edac_teardown(pdev);

	edac_device_del_device(edev_ctl->dev);
	edac_device_free_ctl_info(edev_ctl);

	return 0;
}

static const struct of_device_id qcom_kryo_edac_of_match[] = {
	{ .compatible = "qcom,kryo-edac" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_kryo_edac_of_match);

static struct platform_driver qcom_kryo_edac_driver = {
	.probe = qcom_kryo_edac_probe,
	.remove = qcom_kryo_edac_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table	= qcom_kryo_edac_of_match,
	},
};
module_platform_driver(qcom_kryo_edac_driver);

MODULE_DESCRIPTION("QCOM Kryo EDAC driver");
MODULE_LICENSE("GPL v2");
