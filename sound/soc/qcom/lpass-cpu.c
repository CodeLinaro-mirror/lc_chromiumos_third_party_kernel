// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2010-2011,2013-2015 The Linux Foundation. All rights reserved.
 *
 * lpass-cpu.c -- ALSA SoC CPU DAI driver for QTi LPASS
 */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include "lpass-lpaif-reg.h"
#include "lpass.h"

static int lpass_cpu_daiops_set_sysclk(struct snd_soc_dai *dai, int clk_id,
		unsigned int freq, int dir)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	struct lpass_dai *dai_data = drvdata->dai_priv[dai->driver->id];
	int ret = 0;

	switch (clk_id) {
	case LPASS_MCLK0... LPASS_MCLK2:
		ret = clk_set_rate(dai_data->mclk, freq);
		if (ret)
			dev_err(dai->dev, "error setting mclk%d to %u: %d\n",
				clk_id, freq, ret);
		break;
	default:
		ret = clk_set_rate(dai_data->osr_clk, freq);
		if (ret)
			dev_err(dai->dev, "error setting osrclk to %u: %d\n",
				freq, ret);
	}
	return ret;
}

static int lpass_cpu_daiops_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	struct lpass_dai *dai_data = drvdata->dai_priv[dai->driver->id];
	int ret;

	if (dai_data->osr_clk != NULL) {
		ret = clk_prepare_enable(dai_data->osr_clk);
		if (ret) {
			dev_err(dai->dev,
				"error in enabling mi2s osr clk: %d\n", ret);
			return ret;
		}
	}

	if (dai_data->mclk != NULL) {
		ret = clk_prepare_enable(dai_data->mclk);
		if (ret) {
			dev_err(dai->dev, "error in enabling mi2s mclk: %d\n", ret);
			return ret;
		}
	}

	ret = clk_prepare_enable(dai_data->bit_clk);
	if (ret) {
		dev_err(dai->dev, "error in enabling mi2s bit clk: %d\n", ret);
		clk_disable_unprepare(dai_data->osr_clk);
		return ret;
	}

	return 0;
}

static void lpass_cpu_daiops_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	struct lpass_dai *dai_data = drvdata->dai_priv[dai->driver->id];

	clk_disable_unprepare(dai_data->bit_clk);
	clk_disable_unprepare(dai_data->mclk);
	clk_disable_unprepare(dai_data->osr_clk);
}

static int lpass_cpu_daiops_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	struct lpass_dai *dai_data = drvdata->dai_priv[dai->driver->id];
	struct lpass_variant *v = drvdata->variant;
	snd_pcm_format_t format = params_format(params);
	unsigned int channels = params_channels(params);
	unsigned int rate = params_rate(params);
	unsigned int regval;
	int bitwidth, ret;

	bitwidth = snd_pcm_format_width(format);
	if (bitwidth < 0) {
		dev_err(dai->dev, "invalid bit width given: %d\n", bitwidth);
		return bitwidth;
	}

	/* default to Loopback disable & wssrc internal */
	regval = dai_data->loopback << (LPAIF_I2SCTL(v, LOOPBACK_SHIFT));
	regval |= dai_data->wssrc << (LPAIF_I2SCTL(v, WSSRC_SHIFT));

	switch (bitwidth) {
	case 16:
		regval |= LPAIF_I2SCTL(v, BITWIDTH_16);
		break;
	case 24:
		regval |= LPAIF_I2SCTL(v, BITWIDTH_24);
		break;
	case 32:
		regval |= LPAIF_I2SCTL(v, BITWIDTH_32);
		break;
	default:
		dev_err(dai->dev, "invalid bitwidth given: %d\n", bitwidth);
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		switch (channels) {
		case 1:
			regval |= LPAIF_I2SCTL(v, SPKMODE_SD0);
			regval |= LPAIF_I2SCTL(v, SPKMONO_MONO);
			break;
		case 2:
			regval |= LPAIF_I2SCTL(v, SPKMODE_SD0);
			regval |= LPAIF_I2SCTL(v, SPKMONO_STEREO);
			break;
		case 4:
			regval |= LPAIF_I2SCTL(v, SPKMODE_QUAD01);
			regval |= LPAIF_I2SCTL(v, SPKMONO_STEREO);
			break;
		case 6:
			regval |= LPAIF_I2SCTL(v, SPKMODE_6CH);
			regval |= LPAIF_I2SCTL(v, SPKMONO_STEREO);
			break;
		case 8:
			regval |= LPAIF_I2SCTL(v, SPKMODE_8CH);
			regval |= LPAIF_I2SCTL(v, SPKMONO_STEREO);
			break;
		default:
			dev_err(dai->dev, "invalid channels given: %u\n",
				channels);
			return -EINVAL;
		}
	} else {
		switch (channels) {
		case 1:
			regval |= LPAIF_I2SCTL(v, MICMODE_SD0);
			regval |= LPAIF_I2SCTL(v, MICMONO_MONO);
			break;
		case 2:
			regval |= LPAIF_I2SCTL(v, MICMODE_SD0);
			regval |= LPAIF_I2SCTL(v, MICMONO_STEREO);
			break;
		case 4:
			regval |= LPAIF_I2SCTL(v, MICMODE_QUAD01);
			regval |= LPAIF_I2SCTL(v, MICMONO_STEREO);
			break;
		case 6:
			regval |= LPAIF_I2SCTL(v, MICMODE_6CH);
			regval |= LPAIF_I2SCTL(v, MICMONO_STEREO);
			break;
		case 8:
			regval |= LPAIF_I2SCTL(v, MICMODE_8CH);
			regval |= LPAIF_I2SCTL(v, MICMONO_STEREO);
			break;
		default:
			dev_err(dai->dev, "invalid channels given: %u\n",
				channels);
			return -EINVAL;
		}
	}

	ret = regmap_write(drvdata->lpaif_map,
			   LPAIF_I2SCTL_REG(drvdata->variant, dai->driver->id),
			   regval);
	if (ret) {
		dev_err(dai->dev, "error writing to i2sctl reg: %d\n", ret);
		return ret;
	}

	/* Overwrite spk & mic mode bits with device tree value if specified */
	if (dai_data->spkmode != 0) {
		regval = dai_data->spkmode << (LPAIF_I2SCTL(v, SPKMODE_SHIFT));
		ret = regmap_update_bits(drvdata->lpaif_map,
			LPAIF_I2SCTL_REG(drvdata->variant, dai->driver->id),
			LPAIF_I2SCTL(v, SPKMODE_MASK), regval);
		if (ret)
			dev_err(dai->dev, "error writing to i2sctl reg: %d\n",
				ret);
	}


	if (dai_data->micmode != 0) {
		regval = dai_data->micmode << (LPAIF_I2SCTL(v, MICMODE_SHIFT));
		ret = regmap_update_bits(drvdata->lpaif_map,
			LPAIF_I2SCTL_REG(drvdata->variant, dai->driver->id),
			LPAIF_I2SCTL(v, MICMODE_MASK), regval);
		if (ret)
			dev_err(dai->dev, "error writing to i2sctl reg: %d\n",
				ret);
	}

	ret = clk_set_rate(dai_data->bit_clk, rate * bitwidth * 2);
	if (ret) {
		dev_err(dai->dev, "error setting mi2s bitclk to %u: %d\n",
			rate * bitwidth * 2, ret);
		return ret;
	}

	return 0;
}

static int lpass_cpu_daiops_hw_free(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	int ret;

	ret = regmap_write(drvdata->lpaif_map,
			   LPAIF_I2SCTL_REG(drvdata->variant, dai->driver->id),
			   0);
	if (ret)
		dev_err(dai->dev, "error writing to i2sctl reg: %d\n", ret);

	return ret;
}

static int lpass_cpu_daiops_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	struct lpass_variant *v = drvdata->variant;
	int ret;
	unsigned int val, mask;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		val = LPAIF_I2SCTL(v, SPKEN_ENABLE);
		mask = LPAIF_I2SCTL(v, SPKEN_MASK);
	} else  {
		val = LPAIF_I2SCTL(v, MICEN_ENABLE);
		mask = LPAIF_I2SCTL(v, MICEN_MASK);
	}

	ret = regmap_update_bits(drvdata->lpaif_map,
			LPAIF_I2SCTL_REG(drvdata->variant, dai->driver->id),
			mask, val);
	if (ret)
		dev_err(dai->dev, "error writing to i2sctl reg: %d\n", ret);

	return ret;
}

static int lpass_cpu_daiops_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	struct lpass_variant *v = drvdata->variant;
	int ret = -EINVAL;
	unsigned int val, mask;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			val = LPAIF_I2SCTL(v, SPKEN_ENABLE);
			mask = LPAIF_I2SCTL(v, SPKEN_MASK);
		} else  {
			val = LPAIF_I2SCTL(v, MICEN_ENABLE);
			mask = LPAIF_I2SCTL(v, MICEN_MASK);
		}

		ret = regmap_update_bits(drvdata->lpaif_map,
				LPAIF_I2SCTL_REG(drvdata->variant,
						dai->driver->id),
				mask, val);
		if (ret)
			dev_err(dai->dev, "error writing to i2sctl reg: %d\n",
				ret);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			val = LPAIF_I2SCTL(v, SPKEN_DISABLE);
			mask = LPAIF_I2SCTL(v, SPKEN_MASK);
		} else  {
			val = LPAIF_I2SCTL(v, MICEN_DISABLE);
			mask = LPAIF_I2SCTL(v, MICEN_MASK);
		}

		ret = regmap_update_bits(drvdata->lpaif_map,
				LPAIF_I2SCTL_REG(drvdata->variant,
						dai->driver->id),
				mask, val);
		if (ret)
			dev_err(dai->dev, "error writing to i2sctl reg: %d\n",
				ret);
		break;
	}

	return ret;
}

const struct snd_soc_dai_ops asoc_qcom_lpass_cpu_dai_ops = {
	.set_sysclk	= lpass_cpu_daiops_set_sysclk,
	.startup	= lpass_cpu_daiops_startup,
	.shutdown	= lpass_cpu_daiops_shutdown,
	.hw_params	= lpass_cpu_daiops_hw_params,
	.hw_free	= lpass_cpu_daiops_hw_free,
	.prepare	= lpass_cpu_daiops_prepare,
	.trigger	= lpass_cpu_daiops_trigger,
};
EXPORT_SYMBOL_GPL(asoc_qcom_lpass_cpu_dai_ops);

int asoc_qcom_lpass_cpu_dai_probe(struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	int ret;

	/* ensure audio hardware is disabled */
	ret = regmap_write(drvdata->lpaif_map,
			LPAIF_I2SCTL_REG(drvdata->variant, dai->driver->id), 0);
	if (ret)
		dev_err(dai->dev, "error writing to i2sctl reg: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(asoc_qcom_lpass_cpu_dai_probe);

static const struct snd_soc_component_driver lpass_cpu_comp_driver = {
	.name = "lpass-cpu",
};

static bool lpass_cpu_regmap_writeable(struct device *dev, unsigned int reg)
{
	struct lpass_data *drvdata = dev_get_drvdata(dev);
	struct lpass_variant *v = drvdata->variant;
	int i;

	for (i = 0; i < v->i2s_ports; ++i)
		if (reg == LPAIF_I2SCTL_REG(v, i))
			return true;

	for (i = 0; i < v->irq_ports; ++i) {
		if (reg == LPAIF_IRQEN_REG(v, i))
			return true;
		if (reg == LPAIF_IRQCLEAR_REG(v, i))
			return true;
	}

	for (i = 0; i < v->rdma_channels; ++i) {
		if (reg == LPAIF_RDMACTL_REG(v, i))
			return true;
		if (reg == LPAIF_RDMABASE_REG(v, i))
			return true;
		if (reg == LPAIF_RDMABUFF_REG(v, i))
			return true;
		if (reg == LPAIF_RDMAPER_REG(v, i))
			return true;
	}

	for (i = 0; i < v->wrdma_channels; ++i) {
		if (reg == LPAIF_WRDMACTL_REG(v, i + v->wrdma_channel_start))
			return true;
		if (reg == LPAIF_WRDMABASE_REG(v, i + v->wrdma_channel_start))
			return true;
		if (reg == LPAIF_WRDMABUFF_REG(v, i + v->wrdma_channel_start))
			return true;
		if (reg == LPAIF_WRDMAPER_REG(v, i + v->wrdma_channel_start))
			return true;
	}

	return false;
}

static bool lpass_cpu_regmap_readable(struct device *dev, unsigned int reg)
{
	struct lpass_data *drvdata = dev_get_drvdata(dev);
	struct lpass_variant *v = drvdata->variant;
	int i;

	for (i = 0; i < v->i2s_ports; ++i)
		if (reg == LPAIF_I2SCTL_REG(v, i))
			return true;

	for (i = 0; i < v->irq_ports; ++i) {
		if (reg == LPAIF_IRQEN_REG(v, i))
			return true;
		if (reg == LPAIF_IRQSTAT_REG(v, i))
			return true;
	}

	for (i = 0; i < v->rdma_channels; ++i) {
		if (reg == LPAIF_RDMACTL_REG(v, i))
			return true;
		if (reg == LPAIF_RDMABASE_REG(v, i))
			return true;
		if (reg == LPAIF_RDMABUFF_REG(v, i))
			return true;
		if (reg == LPAIF_RDMACURR_REG(v, i))
			return true;
		if (reg == LPAIF_RDMAPER_REG(v, i))
			return true;
	}

	for (i = 0; i < v->wrdma_channels; ++i) {
		if (reg == LPAIF_WRDMACTL_REG(v, i + v->wrdma_channel_start))
			return true;
		if (reg == LPAIF_WRDMABASE_REG(v, i + v->wrdma_channel_start))
			return true;
		if (reg == LPAIF_WRDMABUFF_REG(v, i + v->wrdma_channel_start))
			return true;
		if (reg == LPAIF_WRDMACURR_REG(v, i + v->wrdma_channel_start))
			return true;
		if (reg == LPAIF_WRDMAPER_REG(v, i + v->wrdma_channel_start))
			return true;
	}

	return false;
}

static bool lpass_cpu_regmap_volatile(struct device *dev, unsigned int reg)
{
	struct lpass_data *drvdata = dev_get_drvdata(dev);
	struct lpass_variant *v = drvdata->variant;
	int i;

	for (i = 0; i < v->irq_ports; ++i)
		if (reg == LPAIF_IRQSTAT_REG(v, i))
			return true;

	for (i = 0; i < v->rdma_channels; ++i)
		if (reg == LPAIF_RDMACURR_REG(v, i))
			return true;

	for (i = 0; i < v->wrdma_channels; ++i)
		if (reg == LPAIF_WRDMACURR_REG(v, i + v->wrdma_channel_start))
			return true;

	return false;
}

static struct regmap_config lpass_cpu_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.writeable_reg = lpass_cpu_regmap_writeable,
	.readable_reg = lpass_cpu_regmap_readable,
	.volatile_reg = lpass_cpu_regmap_volatile,
	.cache_type = REGCACHE_FLAT,
};

static void of_qcom_parse_dai_data(struct device *dev,
				    struct lpass_data *drvdata)
{
	struct device_node *node;
	struct lpass_dai *dai;
	int ret;

	for_each_child_of_node(dev->of_node, node) {
		int id;

		ret = of_property_read_u32(node, "id", &id);
		if (ret || id < 0 || id >= LPASS_MAX_MI2S_PORTS) {
			dev_err(dev, "valid dai id not found:%d\n", ret);
			continue;
		}

		dai = drvdata->dai_priv[id];
		switch (id) {
		case MI2S_PRIMARY... MI2S_QUATERNARY:
			 /* MI2S specific properties */
			ret = of_property_read_string(node, "qcom,osrclk-name",
						      &dai->osrclk_name);
			if (ret) {
				dev_warn(dev, "dai:%d osrclk not defined", id);
			}

                        ret = of_property_read_string(node, "qcom,mclk-name",
                                                      &dai->mclk_name);
                        if (ret) {
                                dev_warn(dev, "dai:%d mclk not defined", id);
                        }

			ret = of_property_read_string(node, "qcom,bitclk-name",
							&dai->bitclk_name);
			if (ret) {
				dev_err(dev, "dai:%d bitclk not defined", id);
			}

			ret = of_property_read_u32(node,"qcom,spkmode-mask",
						   &dai->spkmode);

			ret = of_property_read_u32(node,"qcom,micmode-mask",
						   &dai->micmode);

			ret = of_property_read_u32(node,"qcom,wssrc-mask",
						   &dai->wssrc);

			ret = of_property_read_u32(node,"qcom,loopback-mask",
						   &dai->loopback);

			break;
		case IPQ806X_LPAIF_I2S_PORT_MI2S:
			/* We have hardcoded clock name for IPQ806X */
			dai->osrclk_name = "mi2s-osr-clk";
			dai->bitclk_name = "mi2s-bit-clk";
			break;
		default:
			dev_err(dev, "valid dai not found:%d\n", id);
			break;
		}
	}
}

static int lpass_init_dai_clocks(struct device *dev,
			   struct lpass_data *drvdata)
{
	struct lpass_dai *dai;
	struct lpass_variant *variant = drvdata->variant;
	int i;

	for (i = 0; i < variant->num_dai; i++) {

		dai = drvdata->dai_priv[i];

		dai->osr_clk = devm_clk_get_optional(dev, dai->osrclk_name);
		dai->mclk = devm_clk_get_optional(dev, dai->mclk_name);
		dai->bit_clk = devm_clk_get(dev, dai->bitclk_name);
	}

	return 0;
}

int asoc_qcom_lpass_cpu_platform_probe(struct platform_device *pdev)
{
	struct lpass_data *drvdata;
	struct device_node *dsp_of_node;
	struct resource *res;
	struct lpass_variant *variant;
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	int ret, i;

	dsp_of_node = of_parse_phandle(pdev->dev.of_node, "qcom,adsp", 0);
	if (dsp_of_node) {
		dev_err(&pdev->dev, "DSP exists and holds audio resources\n");
		return -EBUSY;
	}

	drvdata = devm_kzalloc(&pdev->dev, sizeof(struct lpass_data),
			GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;
	platform_set_drvdata(pdev, drvdata);

	match = of_match_device(dev->driver->of_match_table, dev);
	if (!match || !match->data)
		return -EINVAL;

	drvdata->variant = (struct lpass_variant *)match->data;
	variant = drvdata->variant;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lpass-lpaif");

	drvdata->lpaif = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR((void const __force *)drvdata->lpaif)) {
		dev_err(&pdev->dev, "error mapping reg resource: %ld\n",
				PTR_ERR((void const __force *)drvdata->lpaif));
		return PTR_ERR((void const __force *)drvdata->lpaif);
	}

	lpass_cpu_regmap_config.max_register = LPAIF_WRDMAPER_REG(variant,
						variant->wrdma_channels +
						variant->wrdma_channel_start);

	drvdata->lpaif_map = devm_regmap_init_mmio(&pdev->dev, drvdata->lpaif,
			&lpass_cpu_regmap_config);
	if (IS_ERR(drvdata->lpaif_map)) {
		dev_err(&pdev->dev, "error initializing regmap: %ld\n",
			PTR_ERR(drvdata->lpaif_map));
		return PTR_ERR(drvdata->lpaif_map);
	}

	if (variant->init)
		variant->init(pdev);

	for (i = 0; i < variant->num_dai; i++) {
		drvdata->dai_priv[i] = devm_kzalloc(dev,
						sizeof(struct lpass_dai),
						GFP_KERNEL);
	}

	/* parse dai data from dts */
	of_qcom_parse_dai_data(dev, drvdata);

	ret = lpass_init_dai_clocks(dev, drvdata);
	if (ret) {
		dev_err(&pdev->dev, "error intializing dai clock: %d\n", ret);
		return ret;
	}

	drvdata->ahbix_clk = devm_clk_get_optional(&pdev->dev, "ahbix-clk");
	if (IS_ERR(drvdata->ahbix_clk)) {
		dev_err(&pdev->dev, "error getting ahbix-clk: %ld\n",
			PTR_ERR(drvdata->ahbix_clk));
		return PTR_ERR(drvdata->ahbix_clk);
	}

	if (drvdata->ahbix_clk != NULL) {
		ret = clk_set_rate(drvdata->ahbix_clk,
				   LPASS_AHBIX_CLOCK_FREQUENCY);
		if (ret) {
			dev_err(&pdev->dev,
				"error setting rate on ahbix_clk: %d\n", ret);
			return ret;
		}

		dev_dbg(&pdev->dev, "set ahbix_clk rate to %lu\n",
			clk_get_rate(drvdata->ahbix_clk));

		ret = clk_prepare_enable(drvdata->ahbix_clk);
		if (ret) {
			dev_err(&pdev->dev,
				"error enabling ahbix_clk: %d\n", ret);
			return ret;
		}
	}

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &lpass_cpu_comp_driver,
					      variant->dai_driver,
					      variant->num_dai);
	if (ret) {
		dev_err(&pdev->dev, "error registering cpu driver: %d\n", ret);
		goto err_clk;
	}

	ret = asoc_qcom_lpass_platform_register(pdev);
	if (ret) {
		dev_err(&pdev->dev, "error registering platform driver: %d\n",
			ret);
		goto err_clk;
	}

	return 0;

err_clk:
	clk_disable_unprepare(drvdata->ahbix_clk);
	return ret;
}
EXPORT_SYMBOL_GPL(asoc_qcom_lpass_cpu_platform_probe);

int asoc_qcom_lpass_cpu_platform_remove(struct platform_device *pdev)
{
	struct lpass_data *drvdata = platform_get_drvdata(pdev);

	if (drvdata->variant->exit)
		drvdata->variant->exit(pdev);

	clk_disable_unprepare(drvdata->ahbix_clk);

	return 0;
}
EXPORT_SYMBOL_GPL(asoc_qcom_lpass_cpu_platform_remove);

MODULE_DESCRIPTION("QTi LPASS CPU Driver");
MODULE_LICENSE("GPL v2");
