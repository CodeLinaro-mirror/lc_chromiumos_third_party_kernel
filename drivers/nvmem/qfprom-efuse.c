// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/nvmem-provider.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#define QFPROM_BLOW_STATUS_BUSY 0x1
#define QFPROM_BLOW_STATUS_READY 0x0

/* Blow timer clock frequency in Mhz for 10nm LPe technology */
#define QFPROM_BLOW_TIMER_OFFSET 0x03c
#define QFPROM_BLOW_TIMER_RESET_VALUE 0x0

/* Amount of time required to hold charge to blow fuse in micro-seconds */
#define QFPROM_FUSE_BLOW_POLL_PERIOD 100
#define QFPROM_BLOW_STATUS_OFFSET 0x048

#define QFPROM_ACCEL_OFFSET 0x044

/**
 * struct qfprom_efuse_platform_data - structure holding qfprom-efuse
 * platform data
 *
 * @name: qfprom-efuse compatible name
 * @fuse_blow_time_in_us: Should contain the wait time when doing the fuse blow
 * @accel_value: Should contain qfprom accel value
 * @accel_reset_value: The reset value of qfprom accel value
 * @qfprom_blow_timer_value: The timer value of qfprom when doing efuse blow
 * @qfprom_blow_reset_freq: The frequency required to set when fuse blowing
 * is done
 * @qfprom_blow_set_freq: The frequency required to set when we start the
 * fuse blowing
 * @qfprom_max_vol: max voltage required to set fuse blow
 * @qfprom_min_vol: min voltage required to set fuse blow
 */
struct qfprom_efuse_platform_data {
	const char *name;
	u8 fuse_blow_time_in_us;
	u32 accel_value;
	u32 accel_reset_value;
	u32 qfprom_blow_timer_value;
	u32 qfprom_blow_reset_freq;
	u32 qfprom_blow_set_freq;
	u32 qfprom_max_vol;
	u32 qfprom_min_vol;
};

/**
 * struct qfprom_efuse_priv - structure holding qfprom-efuse attributes
 *
 * @qfpbase: iomapped memory space for qfprom base
 * @qfpraw: iomapped memory space for qfprom raw fuse region
 * @qfpmap: iomapped memory space for qfprom fuse blow timer

 * @dev: qfprom device structure
 * @secclk: clock supply
 * @vcc: regulator supply

 * @qfpraw_start: qfprom raw fuse start region
 * @qfpraw_end: qfprom raw fuse end region
 * @qfprom_efuse_platform_data: qfprom platform data
 */
struct qfprom_efuse_priv {
	void __iomem *qfpbase;
	void __iomem *qfpraw;
	void __iomem *qfpmap;
	struct device *dev;
	struct clk *secclk;
	struct regulator *vcc;
	resource_size_t qfpraw_start;
	resource_size_t qfpraw_end;
	struct qfprom_efuse_platform_data efuse;
};

/*
 * restore the gcc_sec_ctrl_clk frequency to default value(19.2 MHz)
 */
static int qfprom_reset_clock_settings(const struct qfprom_efuse_priv *priv)
{
	int ret;

	ret = clk_set_rate(priv->secclk, priv->efuse.qfprom_blow_reset_freq);
	if (ret) {
		dev_err(priv->dev, "clk_set_rate() failed to enable secclk\n");
		return ret;
	}

	return 0;
}

/*
 * set the gcc_sec_ctrl_clk to 4.8 MHz
 */
static int qfprom_set_clock_settings(const struct qfprom_efuse_priv *priv)
{
	int ret;

	ret = clk_set_rate(priv->secclk, priv->efuse.qfprom_blow_set_freq);

	if (ret) {
		dev_err(priv->dev, "clk_set_rate() failed to enable secclk\n");
		return ret;
	}

	return 0;
}

/*
 * set and reset the voltage for 1.8V and OFF(0V) on VDD_QFPROM (LDO11)
 */
static int qfprom_set_voltage_settings(const struct qfprom_efuse_priv *priv,
				       int min_uV, int max_uV)
{
	int ret;

	ret = regulator_set_voltage(priv->vcc, min_uV, max_uV);

	if (ret) {
		dev_err(priv->dev, "regulator_set_voltage() failed!\n");
		return ret;
	}

	ret = regulator_enable(priv->vcc);
	if (ret) {
		dev_err(priv->dev, "failed to enable regulator\n");
		return ret;
	}

	return 0;
}

/*
 * resets the value of the blow timer, accel register and the clock
 * and voltage settings
 */
static int qfprom_disable_fuse_blowing(const struct qfprom_efuse_priv *priv)
{
	int ret;

	ret = qfprom_set_voltage_settings(priv, 0, priv->efuse.qfprom_max_vol);
	if (ret) {
		dev_err(priv->dev, "qfprom_set_voltage_settings failed\n");
		return ret;
	}

	ret = qfprom_reset_clock_settings(priv);
	if (ret) {
		dev_err(priv->dev, "qfprom_reset_clock_settings failed\n");
		return ret;
	}

	writel(QFPROM_BLOW_TIMER_RESET_VALUE, priv->qfpmap +
		  QFPROM_BLOW_TIMER_OFFSET);
	writel(priv->efuse.accel_reset_value,
	       priv->qfpmap + QFPROM_ACCEL_OFFSET);

	return 0;
}

/*
 * sets the value of the blow timer, accel register and the clock
 * and voltage settings
 */
static int qfprom_enable_fuse_blowing(const struct qfprom_efuse_priv *priv)
{
	int ret;

	ret = qfprom_disable_fuse_blowing(priv);
	if (ret) {
		dev_err(priv->dev, "qfprom_disable_fuse_blowing()\n");
		return ret;
	}

	writel(priv->efuse.qfprom_blow_timer_value, priv->qfpmap +
	       QFPROM_BLOW_TIMER_OFFSET);
	writel(priv->efuse.accel_value, priv->qfpmap + QFPROM_ACCEL_OFFSET);

	ret = qfprom_set_clock_settings(priv);
	if (ret) {
		dev_err(priv->dev, "qpfrom_set_clock_settings()\n");
		return ret;
	}

	ret = qfprom_set_voltage_settings(priv, priv->efuse.qfprom_min_vol,
					  priv->efuse.qfprom_max_vol);
	if (ret) {
		dev_err(priv->dev, "qfprom_set_voltage_settings()\n");
		return ret;
	}

	return 0;
}

/*
 * verifying to make sure address being written or read is from qfprom
 * raw address range
 */
bool addr_in_qfprom_range(const struct qfprom_efuse_priv *priv, u32 reg,
			  size_t bytes)
{
	if (((reg + bytes) > reg) && (reg >= priv->qfpraw_start) &&
	    ((reg + bytes) <= priv->qfpraw_end)) {
		return 1;
	}

	return 0;
}

/*
 * API for reading from raw qfprom region
 */
static int qfprom_efuse_reg_read(void *context, unsigned int reg, void *_val,
				 size_t bytes)
{
	struct qfprom_efuse_priv *priv = context;
	u32 *value = _val;
	u32 align_check;
	int i = 0, words = bytes / 4;

	dev_info(priv->dev,
		 "reading raw qfprom region offset: 0x%08x of size: %zd\n",
		 reg, bytes);

	if (bytes % 4 != 0x00) {
		dev_err(priv->dev,
			"Bytes: %zd to read should be word align\n",
			bytes);
		return -EINVAL;
	}

	if (!addr_in_qfprom_range(priv, reg, bytes)) {
		dev_err(priv->dev,
			"Invalid qfprom raw region offset 0x%08x & bytes %zd\n",
			reg, bytes);
		return -EINVAL;
	}

	align_check = (reg & 0xF);

	if (((align_check & ~3) == align_check) && value != NULL)
		while (words--)
			*value++ = readl(priv->qfpbase + reg + (i++ * 4));

	else
		dev_err(priv->dev,
			"Invalid input parameter 0x%08x fuse blow address\n",
			reg);

	return 0;
}

/*
 * API for writing to raw qfprom region - fuse blowing
 * returns success or failure code as per the conditions
 */
static int qfprom_efuse_reg_write(void *context, unsigned int reg, void *_val,
				  size_t bytes)
{
	struct qfprom_efuse_priv *priv = context;
	u32 *value = _val;
	u32 align_check;
	u32 blow_status = QFPROM_BLOW_STATUS_BUSY;
	int ret;
	int i = 0, words = bytes / 4;

	dev_info(priv->dev,
		 "writing to raw qfprom region : 0x%08x of size: %zd\n",
		 reg, bytes);

	if (bytes % 4 != 0x00) {
		dev_err(priv->dev, "Bytes: %zd should be word align\n", bytes);
		return -EINVAL;
	}

	if (!addr_in_qfprom_range(priv, reg, bytes)) {
		dev_err(priv->dev,
			"Invalid qfprom raw region offset 0x%08x & bytes %zd\n",
			 reg, bytes);
		return -EINVAL;
	}

	align_check = (reg & 0xF);
	if (value && ((align_check & ~3) == align_check)) {
		ret = qfprom_enable_fuse_blowing(priv);
		if (ret) {
			dev_err(priv->dev, "qfprom_enable_fuse_blowing");
			return ret;
		}

		ret = readl_relaxed_poll_timeout(priv->qfpmap +
				QFPROM_BLOW_STATUS_OFFSET, blow_status,
				(blow_status  != QFPROM_BLOW_STATUS_BUSY),
				QFPROM_FUSE_BLOW_POLL_PERIOD,
				priv->efuse.fuse_blow_time_in_us);

		if (ret) {
			dev_err(priv->dev, "Timeout blow status ready\n");
			return ret;
		}

		if (blow_status == QFPROM_BLOW_STATUS_READY) {
			while (words--)
				writel(*value++,
				       priv->qfpbase + reg + (i++ * 4));

			ret = readl_relaxed_poll_timeout(priv->qfpmap +
				QFPROM_BLOW_STATUS_OFFSET, blow_status,
				(blow_status  != QFPROM_BLOW_STATUS_BUSY),
				QFPROM_FUSE_BLOW_POLL_PERIOD,
				priv->efuse.fuse_blow_time_in_us);

			if (ret) {
				dev_err(priv->dev, "Timeout blow-status ready");
				return ret;
			}
		}

		ret = qfprom_disable_fuse_blowing(priv);
		if (ret)
			return ret;
	} else {
		dev_err(priv->dev, "Invalid input parameter fuse blow address");
		return -EINVAL;
	}

	dev_info(priv->dev, "written successfully raw qfprom region\n");

	return 0;
}

static int qfprom_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *qfpbase, *qfpraw, *qfpmap;
	struct nvmem_device *nvmem;
	struct nvmem_config *econfig;
	struct qfprom_efuse_priv *priv;
	const struct qfprom_efuse_platform_data *drvdata;
	int ret;

	dev_info(&pdev->dev, "[%s]: Invoked\n", __func__);

	drvdata = of_device_get_match_data(&pdev->dev);
	if (!drvdata)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->efuse.fuse_blow_time_in_us = drvdata->fuse_blow_time_in_us;
	priv->efuse.accel_value = drvdata->accel_value;
	priv->efuse.accel_reset_value = drvdata->accel_reset_value;
	priv->efuse.qfprom_blow_timer_value = drvdata->qfprom_blow_timer_value;
	priv->efuse.qfprom_blow_reset_freq = drvdata->qfprom_blow_reset_freq;
	priv->efuse.qfprom_blow_set_freq = drvdata->qfprom_blow_set_freq;
	priv->efuse.qfprom_max_vol = drvdata->qfprom_max_vol;
	priv->efuse.qfprom_min_vol = drvdata->qfprom_min_vol;
	priv->dev = dev;

	qfpbase = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	priv->qfpbase = devm_ioremap_resource(dev, qfpbase);
	if (IS_ERR(priv->qfpbase)) {
		ret = PTR_ERR(priv->qfpbase);
		goto err;
	}

	qfpraw = platform_get_resource(pdev, IORESOURCE_MEM, 1);

	priv->qfpraw = devm_ioremap_resource(dev, qfpraw);
	if (IS_ERR(priv->qfpraw)) {
		ret = PTR_ERR(priv->qfpraw);
		goto err;
	}

	priv->qfpraw_start = qfpraw->start - qfpbase->start;
	priv->qfpraw_end = qfpraw->end - qfpbase->start;

	qfpmap = platform_get_resource(pdev, IORESOURCE_MEM, 2);

	priv->qfpmap = devm_ioremap_resource(dev, qfpmap);
	if (IS_ERR(priv->qfpmap)) {
		ret = PTR_ERR(priv->qfpmap);
		goto err;
	}

	priv->vcc = devm_regulator_get(&pdev->dev, "vcc");
	if (IS_ERR(priv->vcc)) {
		ret = PTR_ERR(priv->vcc);
		if (ret == -ENODEV)
			ret = -EPROBE_DEFER;

		goto err;
	}

	priv->secclk = devm_clk_get(dev, "secclk");
	if (IS_ERR(priv->secclk)) {
		ret = PTR_ERR(priv->secclk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "secclk error getting : %d\n", ret);
		goto err;
	}

	ret = clk_prepare_enable(priv->secclk);
	if (ret) {
		dev_err(dev, "clk_prepare_enable() failed\n");
		goto err;
	}

	econfig = devm_kzalloc(dev, sizeof(*econfig), GFP_KERNEL);
	if (!econfig)
		return -ENOMEM;

	econfig->dev = dev;
	econfig->name = "qfprom-efuse";
	econfig->stride = 1;
	econfig->word_size = 1;
	econfig->reg_read = qfprom_efuse_reg_read;
	econfig->reg_write = qfprom_efuse_reg_write;
	econfig->size = resource_size(qfpraw);
	econfig->priv = priv;

	nvmem = devm_nvmem_register(dev, econfig);

	return PTR_ERR_OR_ZERO(nvmem);

err:
	clk_disable_unprepare(priv->secclk);
	return ret;
}

static const struct qfprom_efuse_platform_data sc7180_qfp_efuse_data = {
	.name = "sc7180-qfprom-efuse",
	.fuse_blow_time_in_us = 100,
	.accel_value = 0xD10,
	.accel_reset_value = 0x800,
	.qfprom_blow_timer_value = 25,
	.qfprom_blow_reset_freq = 19200000,
	.qfprom_blow_set_freq = 4800000,
	.qfprom_max_vol = 1904000,
	.qfprom_min_vol = 1800000,
};

static const struct of_device_id qfprom_efuse_of_match[] = {
	{
		.compatible = "qcom,sc7180-qfprom-efuse",
		.data = &sc7180_qfp_efuse_data
	},
	{/* sentinel */},
};

MODULE_DEVICE_TABLE(of, qfprom_efuse_of_match);

static struct platform_driver qfprom_efuse_driver = {
	.probe = qfprom_efuse_probe,
	.driver = {
		.name = "sc7180-qfprom-efuse",
		.of_match_table = qfprom_efuse_of_match,
	},
};

module_platform_driver(qfprom_efuse_driver);
MODULE_DESCRIPTION("QTI QFPROM Efuse driver");
MODULE_LICENSE("GPL v2");
