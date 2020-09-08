// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 *
 * lpass-hdmi.c -- ALSA SoC HDMI-CPU DAI driver for QTi LPASS HDMI
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
#include <dt-bindings/sound/sc7180-lpass.h>
#include "lpass-lpaif-reg.h"
#include "lpass.h"
#include "lpass-hdmi.h"

int lpass_hdmi_init_bitfields(struct device *dev, struct regmap *map)
{
	struct lpass_data *drvdata = dev_get_drvdata(dev);
	struct lpass_variant *v = drvdata->variant;
	struct lpass_hdmi_rsrc *hdmi_rsrc = &v->intf.hdmi_intf;
	unsigned int i;
	struct lpass_hdmi_tx_ctl *tx_ctl;
	struct lpass_hdmitx_legacy *legacy;
	struct lpass_vbit_ctrl *vbit_ctl;
	struct lpass_hdmi_tx_parity *tx_parity;
	struct lpass_dp_metadata_ctl *meta_ctl;
	struct lpass_sstream_ctl *sstream_ctl;
	struct lpass_hdmi_tx_ch_msb *ch_msb;
	struct lpass_hdmi_tx_ch_lsb *ch_lsb;
	struct lpass_hdmitx_dmactl *hdmi_tx_dmactl;

	drvdata->tx_ctl = devm_kzalloc(dev, sizeof(struct lpass_hdmi_tx_ctl),
					GFP_KERNEL);
	if (drvdata->tx_ctl == NULL)
		return -ENOMEM;

	tx_ctl = drvdata->tx_ctl;
	tx_ctl->soft_reset = devm_regmap_field_alloc(dev, map,
				hdmi_rsrc->soft_reset);
	tx_ctl->force_reset = devm_regmap_field_alloc(dev, map,
				hdmi_rsrc->force_reset);
	if (IS_ERR(tx_ctl->soft_reset) || IS_ERR(tx_ctl->force_reset))
		return -EINVAL;

	drvdata->legacy = devm_kzalloc(dev, sizeof(struct lpass_hdmitx_legacy),
					GFP_KERNEL);
	if (drvdata->legacy == NULL)
		return -ENOMEM;

	legacy = drvdata->legacy;
	legacy->legacy_en = devm_regmap_field_alloc(dev, map,
				hdmi_rsrc->legacy_en);
	if (IS_ERR(legacy->legacy_en))
		return -EINVAL;

	drvdata->vbit_ctl = devm_kzalloc(dev, sizeof(struct lpass_vbit_ctrl),
					GFP_KERNEL);
	if (drvdata->vbit_ctl == NULL)
		return -ENOMEM;

	vbit_ctl = drvdata->vbit_ctl;
	vbit_ctl->replace_vbit = devm_regmap_field_alloc(dev, map,
					hdmi_rsrc->replace_vbit);
	vbit_ctl->vbit_stream = devm_regmap_field_alloc(dev, map,
					hdmi_rsrc->vbit_stream);
	if (IS_ERR(vbit_ctl->replace_vbit) || IS_ERR(vbit_ctl->vbit_stream))
		return -EINVAL;

	drvdata->tx_parity = devm_kzalloc(dev,
		sizeof(struct  lpass_hdmi_tx_parity), GFP_KERNEL);

	if (drvdata->tx_parity == NULL)
		return -ENOMEM;

	tx_parity = drvdata->tx_parity;
	tx_parity->calc_en = devm_regmap_field_alloc(dev, map, hdmi_rsrc->calc_en);

	if (IS_ERR(tx_parity->calc_en))
		return -EINVAL;

	drvdata->meta_ctl = devm_kzalloc(dev,
		sizeof(struct lpass_dp_metadata_ctl), GFP_KERNEL);

	if (drvdata->meta_ctl == NULL)
		return -ENOMEM;

	meta_ctl = drvdata->meta_ctl;
	meta_ctl->mute = devm_regmap_field_alloc(dev, map, hdmi_rsrc->mute);
	meta_ctl->as_sdp_cc = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->as_sdp_cc);
	meta_ctl->as_sdp_ct = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->as_sdp_ct);
	meta_ctl->aif_db4 = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->aif_db4);
	meta_ctl->frequency = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->frequency);
	meta_ctl->mst_index = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->mst_index);
	meta_ctl->dptx_index = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->dptx_index);

	if (IS_ERR(meta_ctl->mute) || IS_ERR(meta_ctl->as_sdp_cc) ||
		IS_ERR(meta_ctl->as_sdp_ct) || IS_ERR(meta_ctl->aif_db4) ||
		IS_ERR(meta_ctl->frequency) || IS_ERR(meta_ctl->mst_index) ||
		IS_ERR(meta_ctl->dptx_index))
		return -EINVAL;

	drvdata->sstream_ctl = devm_kzalloc(dev,
		sizeof(struct lpass_sstream_ctl), GFP_KERNEL);
	if (drvdata->sstream_ctl == NULL)
		return -ENOMEM;

	sstream_ctl = drvdata->sstream_ctl;
	sstream_ctl->sstream_en = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->sstream_en);
	sstream_ctl->dma_sel = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->dma_sel);
	sstream_ctl->auto_bbit_en = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->auto_bbit_en);
	sstream_ctl->layout = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->layout);
	sstream_ctl->layout_sp = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->layout_sp);
	sstream_ctl->dp_audio = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->dp_audio);
	sstream_ctl->set_sp_on_en = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->set_sp_on_en);
	sstream_ctl->dp_staffing_en = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->dp_staffing_en);
	sstream_ctl->dp_sp_b_hw_en = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->dp_sp_b_hw_en);

	if (IS_ERR(sstream_ctl->sstream_en) || IS_ERR(sstream_ctl->dma_sel) ||
		IS_ERR(sstream_ctl->auto_bbit_en) ||
		IS_ERR(sstream_ctl->layout) || IS_ERR(sstream_ctl->layout_sp) ||
		IS_ERR(sstream_ctl->dp_audio) ||
		IS_ERR(sstream_ctl->set_sp_on_en) ||
		IS_ERR(sstream_ctl->dp_staffing_en) ||
		IS_ERR(sstream_ctl->dp_sp_b_hw_en))
		return -EINVAL;

	for (i = 0; i < LPASS_MAX_HDMI_DMA_CHANNELS; i++) {
		drvdata->ch_msb[i] = devm_kzalloc(dev,
			sizeof(struct lpass_hdmi_tx_ch_msb), GFP_KERNEL);
		if (drvdata->ch_msb[i] == NULL)
			return -ENOMEM;

		ch_msb = drvdata->ch_msb[i];

		ch_msb->msb_bits = devm_regmap_field_alloc(dev, map,
							hdmi_rsrc->msb_bits);
		if (IS_ERR(ch_msb->msb_bits))
			return -EINVAL;

		drvdata->ch_lsb[i] = devm_kzalloc(dev,
			sizeof(struct lpass_hdmi_tx_ch_lsb), GFP_KERNEL);
		if (drvdata->ch_lsb[i] == NULL)
			return -ENOMEM;

		ch_lsb = drvdata->ch_lsb[i];
		ch_lsb->lsb_bits = devm_regmap_field_alloc(dev, map,
					hdmi_rsrc->lsb_bits);
		if (IS_ERR(ch_lsb->lsb_bits))
			return -EINVAL;


		drvdata->hdmi_tx_dmactl[i] = devm_kzalloc(dev,
			sizeof(struct lpass_hdmitx_dmactl), GFP_KERNEL);
		if (drvdata->hdmi_tx_dmactl[i] == NULL)
			return -ENOMEM;

		hdmi_tx_dmactl = drvdata->hdmi_tx_dmactl[i];
		hdmi_tx_dmactl->use_hw_chs = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->use_hw_chs);
		hdmi_tx_dmactl->use_hw_usr = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->use_hw_usr);
		hdmi_tx_dmactl->hw_chs_sel = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->hw_chs_sel);
		hdmi_tx_dmactl->hw_usr_sel = devm_regmap_field_alloc(dev, map,
						hdmi_rsrc->hw_usr_sel);
		if (IS_ERR(hdmi_tx_dmactl->use_hw_chs) ||
			IS_ERR(hdmi_tx_dmactl->use_hw_usr) ||
			IS_ERR(hdmi_tx_dmactl->hw_chs_sel) ||
			IS_ERR(hdmi_tx_dmactl->hw_usr_sel))
			return -EINVAL;
	}
	return 0;

}
EXPORT_SYMBOL(lpass_hdmi_init_bitfields);

static int lpass_hdmi_daiops_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	snd_pcm_format_t format = params_format(params);
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	unsigned int ret;
	unsigned int bitwidth;
	unsigned int word_length;
	unsigned int ch_sts_buf0;
	unsigned int ch_sts_buf1;
	unsigned int data_format;
	unsigned int sampling_freq;
	unsigned int ch = 0;

	bitwidth = snd_pcm_format_width(format);
	if (bitwidth < 0) {
		dev_err(dai->dev, "%s invalid bit width given : %d\n",
					__func__, bitwidth);
		return bitwidth;
	}

	switch (bitwidth) {
	case 16:
		word_length = LPASS_DP_AUDIO_BITWIDTH16;
		break;
	case 24:
		word_length = LPASS_DP_AUDIO_BITWIDTH24;
		break;
	default:
		dev_err(dai->dev, "%s invalid bit width given : %d\n",
					__func__, bitwidth);
		return -EINVAL;
	}

	switch (rate) {
	case 32000:
		sampling_freq = LPASS_SAMPLING_FREQ32;
		break;
	case 44100:
		sampling_freq = LPASS_SAMPLING_FREQ44;
		break;
	case 48000:
		sampling_freq = LPASS_SAMPLING_FREQ48;
		break;

	default:
		dev_err(dai->dev, "%s invalid bit width given : %d\n",
					__func__, bitwidth);
		return -EINVAL;
	}
	data_format = LPASS_DATA_FORMAT_LINEAR;
	ch_sts_buf0 = (((data_format << LPASS_DATA_FORMAT_SHIFT) & LPASS_DATA_FORMAT_MASK)
				| ((sampling_freq << LPASS_FREQ_BIT_SHIFT) & LPASS_FREQ_BIT_MASK));
	ch_sts_buf1 = (word_length) & LPASS_WORDLENGTH_MASK;

	ret = regmap_field_write(drvdata->tx_ctl->soft_reset, LPASS_TX_CTL_RESET);
	if (ret) {
		dev_err(dai->dev, "%s error writing to softreset enable : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->tx_ctl->soft_reset, LPASS_TX_CTL_CLEAR);
	if (ret) {
		dev_err(dai->dev, "%s error writing to softreset disable : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->legacy->legacy_en,
				LPASS_HDMITX_LEGACY_DISABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to legacy_en field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->tx_parity->calc_en,
				HDMITX_PARITY_CALC_EN);
	if (ret) {
		dev_err(dai->dev, "%s error writing to tx_parity field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->vbit_ctl->replace_vbit,
					REPLACE_VBIT);
	if (ret) {
		dev_err(dai->dev, "%s error writing to  replace vbit field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->vbit_ctl->vbit_stream,
					LINEAR_PCM_DATA);
	if (ret) {
		dev_err(dai->dev, "%s error writing to vbit stream field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->ch_msb[0]->msb_bits, ch_sts_buf1);
	if (ret) {
		dev_err(dai->dev, "%s error writing to ch_sts_buf1 field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->ch_lsb[0]->lsb_bits, ch_sts_buf0);
	if (ret) {
		dev_err(dai->dev, "%s error writing to ch_sts_buf0 field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->hdmi_tx_dmactl[0]->use_hw_chs,
				HW_MODE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to use_hw_chs field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->hdmi_tx_dmactl[0]->hw_chs_sel,
				SW_MODE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to hw_chs_sel field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->hdmi_tx_dmactl[0]->use_hw_usr,
				HW_MODE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to use_hw_usr field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->hdmi_tx_dmactl[0]->hw_usr_sel,
				SW_MODE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to hw_usr_sel field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->mute,
				LPASS_MUTE_ENABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to mute field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->as_sdp_cc,
				channels - 1);
	if (ret) {
		dev_err(dai->dev, "%s error writing to as_sdp_cc field: %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->as_sdp_ct,
				LPASS_META_DEFAULT_VAL);
	if (ret) {
		dev_err(dai->dev, "%s error writing to as_sdp_ct field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->aif_db4,
				LPASS_META_DEFAULT_VAL);
	if (ret) {
		dev_err(dai->dev, "%s error writing to aif_db4 field: %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->frequency, sampling_freq);
	if (ret) {
		dev_err(dai->dev, "%s error writing to frequency field: %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->mst_index,
				LPASS_META_DEFAULT_VAL);
	if (ret) {
		dev_err(dai->dev, "%s error writing to mst_index : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->dptx_index,
				LPASS_META_DEFAULT_VAL);
	if (ret) {
		dev_err(dai->dev, "%s error writing to dptx_index field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->sstream_en,
				LPASS_SSTREAM_DISABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to sstream_en field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->dma_sel, ch);
	if (ret) {
		dev_err(dai->dev, "%s error writing to dma_sel field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->auto_bbit_en,
				LPASS_SSTREAM_DEFAULT_ENABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to auto_bbit_en field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->layout,
				LPASS_SSTREAM_DEFAULT_DISABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to layout field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->layout_sp,
				LPASS_LAYOUT_SP_DEFAULT);
	if (ret) {
		dev_err(dai->dev, "%s error writing to layout_sp field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->dp_audio,
				LPASS_SSTREAM_DEFAULT_ENABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to dp_audio field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->set_sp_on_en,
				LPASS_SSTREAM_DEFAULT_ENABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to set_sp_on_en field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->dp_sp_b_hw_en,
				LPASS_SSTREAM_DEFAULT_ENABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to dp_sp_b_hw_en field : %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->sstream_ctl->dp_staffing_en,
				LPASS_SSTREAM_DEFAULT_ENABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to dp_staffing_en field: %d\n",
				__func__, ret);
		return ret;
	}
	return ret;
}



static int lpass_hdmi_daiops_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	int ret;

	ret = regmap_field_write(drvdata->sstream_ctl->sstream_en,
					LPASS_SSTREAM_ENABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to sstream_en field: %d\n",
					__func__, ret);
		return ret;
	}

	ret = regmap_field_write(drvdata->meta_ctl->mute,
					LPASS_MUTE_DISABLE);
	if (ret) {
		dev_err(dai->dev, "%s error writing to mute field : %d\n",
				__func__, ret);
		return ret;
	}
	return ret;
}

static int lpass_hdmi_daiops_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_soc_dai *dai)
{
	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
	int ret = -EINVAL;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:

		ret = regmap_field_write(drvdata->sstream_ctl->sstream_en,
					LPASS_SSTREAM_ENABLE);
		if (ret) {
			dev_err(dai->dev, "%s error writing to sstream_en field: %d\n",
				__func__, ret);
			return ret;
		}

		ret = regmap_field_write(drvdata->meta_ctl->mute,
					LPASS_MUTE_DISABLE);
		if (ret) {
			dev_err(dai->dev, "%s error writing to mute field : %d\n",
				__func__, ret);
			return ret;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:

		ret = regmap_field_write(drvdata->sstream_ctl->sstream_en,
					LPASS_SSTREAM_DISABLE);
		if (ret) {
			dev_err(dai->dev, "%s error writing to sstream_en field: %d\n",
				__func__, ret);
			return ret;
		}

		ret = regmap_field_write(drvdata->meta_ctl->mute,
					LPASS_MUTE_ENABLE);
		if (ret) {
			dev_err(dai->dev, "%s error writing to mute field : %d\n",
				__func__, ret);
			return ret;
		}

		ret = regmap_field_write(drvdata->sstream_ctl->dp_audio, 0);
		if (ret) {
			dev_err(dai->dev, "%s error writing to dp_audio field: %d\n",
					__func__, ret);
			return ret;
		}
		break;
	}
	return ret;
}

const struct snd_soc_dai_ops asoc_qcom_lpass_hdmi_dai_ops = {
	.hw_params	= lpass_hdmi_daiops_hw_params,
	.prepare	= lpass_hdmi_daiops_prepare,
	.trigger	= lpass_hdmi_daiops_trigger,
};
EXPORT_SYMBOL_GPL(asoc_qcom_lpass_hdmi_dai_ops);



static bool lpass_hdmi_regmap_writeable(struct device *dev, unsigned int reg)
{
	struct lpass_data *drvdata = dev_get_drvdata(dev);
	struct lpass_variant *v = drvdata->variant;
	int i;

	if (reg == LPASS_HDMI_TX_CTL_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_LEGACY_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_VBIT_CTL_ADDR(v))
		return true;

	for (i = 0; i < v->rdma_channels; i++) {
		if (reg == LPASS_HDMI_TX_CH_LSB_ADDR(v, i))
			return true;
		if (reg == LPASS_HDMI_TX_CH_MSB_ADDR(v, i))
			return true;
		if (reg == LPASS_HDMI_TX_DMA_ADDR(v, i))
			return true;
	}

	if (reg == LPASS_HDMI_TX_PARITY_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_DP_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_SSTREAM_ADDR(v))
		return true;

	if (reg == LPASS_HDMITX_APP_IRQEN_REG(v))
		return true;
	if (reg == LPASS_HDMITX_APP_IRQCLEAR_REG(v))
		return true;

	for (i = 0; i < v->rdma_channels; ++i) {
		if (reg == LPAIF_HDMI_RDMACTL_REG(v, i))
			return true;
		if (reg == LPAIF_HDMI_RDMABASE_REG(v, i))
			return true;
		if (reg == LPAIF_HDMI_RDMABUFF_REG(v, i))
			return true;
		if (reg == LPAIF_HDMI_RDMAPER_REG(v, i))
			return true;

	}
	return false;
}

static bool lpass_hdmi_regmap_readable(struct device *dev, unsigned int reg)
{
	struct lpass_data *drvdata = dev_get_drvdata(dev);
	struct lpass_variant *v = drvdata->variant;
	int i;

	if (reg == LPASS_HDMI_TX_CTL_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_LEGACY_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_VBIT_CTL_ADDR(v))
		return true;

	for (i = 0; i < v->rdma_channels; i++) {
		if (reg == LPASS_HDMI_TX_CH_LSB_ADDR(v, i))
			return true;
		if (reg == LPASS_HDMI_TX_CH_MSB_ADDR(v, i))
			return true;
		if (reg == LPASS_HDMI_TX_DMA_ADDR(v, i))
			return true;
	}

	if (reg == LPASS_HDMI_TX_PARITY_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_DP_ADDR(v))
		return true;
	if (reg == LPASS_HDMI_TX_SSTREAM_ADDR(v))
		return true;

	if (reg == LPASS_HDMITX_APP_IRQEN_REG(v))
		return true;
	if (reg == LPASS_HDMITX_APP_IRQSTAT_REG(v))
		return true;

	for (i = 0; i < v->rdma_channels; ++i) {
		if (reg == LPAIF_HDMI_RDMACTL_REG(v, i))
			return true;
		if (reg == LPAIF_HDMI_RDMABASE_REG(v, i))
			return true;
		if (reg == LPAIF_HDMI_RDMABUFF_REG(v, i))
			return true;
		if (reg == LPAIF_HDMI_RDMAPER_REG(v, i))
			return true;
		if (reg == LPAIF_HDMI_RDMACURR_REG(v, i))
			return true;
	}

	return false;
}

static bool lpass_hdmi_regmap_volatile(struct device *dev, unsigned int reg)
{
	return true;

}
struct regmap_config lpass_hdmi_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.writeable_reg = lpass_hdmi_regmap_writeable,
	.readable_reg = lpass_hdmi_regmap_readable,
	.volatile_reg = lpass_hdmi_regmap_volatile,
	.cache_type = REGCACHE_FLAT,
};
EXPORT_SYMBOL(lpass_hdmi_regmap_config);

MODULE_DESCRIPTION("QTi LPASS HDMI Driver");
MODULE_LICENSE("GPL v2");
