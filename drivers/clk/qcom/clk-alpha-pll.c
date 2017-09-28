/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/clk-provider.h>
#include <linux/regmap.h>
#include <linux/delay.h>

#include "clk-alpha-pll.h"
#include "common.h"

# define PLL_OUTCTRL		BIT(0)
# define PLL_BYPASSNL		BIT(1)
# define PLL_RESET_N		BIT(2)
# define PLL_OFFLINE_REQ	BIT(7)
# define PLL_LOCK_COUNT_SHIFT	8
# define PLL_LOCK_COUNT_MASK	0x3f
# define PLL_BIAS_COUNT_SHIFT	14
# define PLL_BIAS_COUNT_MASK	0x3f
# define PLL_VOTE_FSM_ENA	BIT(20)
# define PLL_FSM_ENA		BIT(20)
# define PLL_VOTE_FSM_RESET	BIT(21)
# define PLL_OFFLINE_ACK	BIT(28)
# define PLL_ACTIVE_FLAG	BIT(30)
# define PLL_LOCK_DET		BIT(31)

# define PLL_POST_DIV_SHIFT	8
# define PLL_POST_DIV_MASK	0xf
# define PLL_ALPHA_EN		BIT(24)
# define PLL_VCO_SHIFT		20
# define PLL_VCO_MASK		0x3

/*
 * Even though 40 bits are present, use only 32 for ease of calculation.
 */
#define ALPHA_REG_BITWIDTH	40
#define ALPHA_BITWIDTH		32
#define ALPHA_16BIT_MASK	0xffff

/* Returns the alpha_pll_clk_ops for pll type */
#define pll_clk_ops(hw)		(alpha_pll_props[to_clk_alpha_pll(hw)->	   \
				 pll_type].ops)

/* Returns the actual register offset for the reg crossponding to pll type */
#define pll_reg(type, reg)	alpha_pll_props[type].reg_offsets[reg]

/* Helpers to return the actual register offset */
#define pll_l(type)		pll_reg(type, PLL_L_VAL)
#define pll_alpha(type)		pll_reg(type, PLL_ALPHA_VAL)
#define pll_alpha_u(type)	pll_reg(type, PLL_ALPHA_VAL_U)
#define pll_user_ctl(type)	pll_reg(type, PLL_USER_CTL)
#define pll_user_ctl_u(type)	pll_reg(type, PLL_USER_CTL_U)
#define pll_cfg_ctl(type)	pll_reg(type, PLL_CONFIG_CTL)
#define pll_test_ctl(type)	pll_reg(type, PLL_TEST_CTL)
#define pll_test_ctl_u(type)	pll_reg(type, PLL_TEST_CTL_U)
#define pll_status(type)	pll_reg(type, PLL_STATUS)
#define pll_cfg_ctl_u(type)	pll_reg(type, PLL_CONFIG_CTL_U)

#define to_clk_alpha_pll(_hw) container_of(to_clk_regmap(_hw), \
					   struct clk_alpha_pll, clkr)

#define to_clk_alpha_pll_postdiv(_hw) container_of(to_clk_regmap(_hw), \
					   struct clk_alpha_pll_postdiv, clkr)

/**
 * Contains the index which will be used for mapping with actual
 * register offset in Alpha PLL
 */
enum {
	PLL_L_VAL,
	PLL_ALPHA_VAL,
	PLL_ALPHA_VAL_U,
	PLL_USER_CTL,
	PLL_USER_CTL_U,
	PLL_CONFIG_CTL,
	PLL_CONFIG_CTL_U,
	PLL_TEST_CTL,
	PLL_TEST_CTL_U,
	PLL_STATUS,
	PLL_MAX_REGS,
};

/**
 * struct alpha_pll_clk_ops - operations for alpha PLL
 * @enable: enable function when HW voting FSM is disabled
 * @disable: disable function when HW voting FSM is disabled
 * @is_enabled: check whether PLL is enabled when HW voting FSM is disabled
 * @hwfsm_enable: check whether PLL is enabled when HW voting FSM is enabled
 * @hwfsm_disable: check whether PLL is disabled when HW voting FSM is enabled
 * @hwfsm_is_enabled: check whether PLL is enabled when HW voting FSM is enabled
 * @recalc_rate: recalculate the rate of PLL by reading mode, L and Alpha Value
 * @round_rate: returns the closest supported rate of PLL
 * @set_rate: change the rate of this clock by actually programming the mode, L
 *	      and Alpha Value registers
 */
struct alpha_pll_clk_ops {
	int		(*enable)(struct clk_hw *hw);
	void		(*disable)(struct clk_hw *hw);
	int		(*is_enabled)(struct clk_hw *hw);
	int		(*hwfsm_enable)(struct clk_hw *hw);
	void		(*hwfsm_disable)(struct clk_hw *hw);
	int		(*hwfsm_is_enabled)(struct clk_hw *hw);
	unsigned long	(*recalc_rate)(struct clk_hw *hw,
				       unsigned long parent_rate);
	long		(*round_rate)(struct clk_hw *hw, unsigned long rate,
				      unsigned long *parent_rate);
	int		(*set_rate)(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate);
};

/**
 * struct alpha_pll_props - contains the various properties which
 *			    will be fixed for PLL type.
 * @reg_offsets: register offsets mapping array
 * @ops: clock operations for alpha PLL
 */
struct alpha_pll_props {
	u8 reg_offsets[PLL_MAX_REGS];
	struct alpha_pll_clk_ops ops;
};

static const struct alpha_pll_props alpha_pll_props[];

static int wait_for_pll(struct clk_alpha_pll *pll, u32 mask, bool inverse,
			const char *action)
{
	u32 val, off = pll->offset;
	int count;
	int ret;
	const char *name = clk_hw_get_name(&pll->clkr.hw);

	ret = regmap_read(pll->clkr.regmap, off, &val);
	if (ret)
		return ret;

	for (count = 100; count > 0; count--) {
		ret = regmap_read(pll->clkr.regmap, off, &val);
		if (ret)
			return ret;
		if (inverse && !(val & mask))
			return 0;
		else if ((val & mask) == mask)
			return 0;

		udelay(1);
	}

	WARN(1, "%s failed to %s!\n", name, action);
	return -ETIMEDOUT;
}

#define wait_for_pll_enable_active(pll) \
	wait_for_pll(pll, PLL_ACTIVE_FLAG, 0, "enable")

#define wait_for_pll_enable_lock(pll) \
	wait_for_pll(pll, PLL_LOCK_DET, 0, "enable")

#define wait_for_pll_disable(pll) \
	wait_for_pll(pll, PLL_ACTIVE_FLAG, 1, "disable")

#define wait_for_pll_offline(pll) \
	wait_for_pll(pll, PLL_OFFLINE_ACK, 0, "offline")

void clk_alpha_pll_configure(struct clk_alpha_pll *pll, struct regmap *regmap,
			     const struct alpha_pll_config *config)
{
	u32 val, mask;
	u32 off = pll->offset;
	u8 type = pll->pll_type;

	regmap_write(regmap, off + pll_l(type), config->l);
	regmap_write(regmap, off + pll_alpha(type), config->alpha);
	regmap_write(regmap, off + pll_cfg_ctl(type), config->config_ctl_val);
	regmap_write(regmap, off + pll_cfg_ctl_u(type),
		     config->config_ctl_hi_val);

	val = config->main_output_mask;
	val |= config->aux_output_mask;
	val |= config->aux2_output_mask;
	val |= config->early_output_mask;
	val |= config->pre_div_val;
	val |= config->post_div_val;
	val |= config->vco_val;

	mask = config->main_output_mask;
	mask |= config->aux_output_mask;
	mask |= config->aux2_output_mask;
	mask |= config->early_output_mask;
	mask |= config->pre_div_mask;
	mask |= config->post_div_mask;
	mask |= config->vco_mask;

	regmap_update_bits(regmap, off + pll_user_ctl(type), mask, val);

	if (pll->flags & SUPPORTS_FSM_MODE)
		qcom_pll_set_fsm_mode(regmap, off, 6, 0);
}

static int alpha_pll_default_hwfsm_enable(struct clk_hw *hw)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	int ret;
	u32 val, off = pll->offset;

	ret = regmap_read(pll->clkr.regmap, off, &val);
	if (ret)
		return ret;

	val |= PLL_FSM_ENA;

	if (pll->flags & SUPPORTS_OFFLINE_REQ)
		val &= ~PLL_OFFLINE_REQ;

	ret = regmap_write(pll->clkr.regmap, off, val);
	if (ret)
		return ret;

	/* Make sure enable request goes through before waiting for update */
	mb();

	return wait_for_pll_enable_active(pll);
}

static void alpha_pll_default_hwfsm_disable(struct clk_hw *hw)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	int ret;
	u32 val, off = pll->offset;

	ret = regmap_read(pll->clkr.regmap, off, &val);
	if (ret)
		return;

	if (pll->flags & SUPPORTS_OFFLINE_REQ) {
		ret = regmap_update_bits(pll->clkr.regmap, off,
					 PLL_OFFLINE_REQ, PLL_OFFLINE_REQ);
		if (ret)
			return;

		ret = wait_for_pll_offline(pll);
		if (ret)
			return;
	}

	/* Disable hwfsm */
	ret = regmap_update_bits(pll->clkr.regmap, off,
				 PLL_FSM_ENA, 0);
	if (ret)
		return;

	wait_for_pll_disable(pll);
}

static int pll_is_enabled(struct clk_hw *hw, u32 mask)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	int ret;
	u32 val, off = pll->offset;

	ret = regmap_read(pll->clkr.regmap, off, &val);
	if (ret)
		return ret;

	return !!(val & mask);
}

static int alpha_pll_default_hwfsm_is_enabled(struct clk_hw *hw)
{
	return pll_is_enabled(hw, PLL_ACTIVE_FLAG);
}

static int alpha_pll_default_is_enabled(struct clk_hw *hw)
{
	return pll_is_enabled(hw, PLL_LOCK_DET);
}

static int alpha_pll_default_enable(struct clk_hw *hw)
{
	int ret;
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	u32 val, mask, off = pll->offset;

	mask = PLL_OUTCTRL | PLL_RESET_N | PLL_BYPASSNL;
	ret = regmap_read(pll->clkr.regmap, off, &val);
	if (ret)
		return ret;

	/* If in FSM mode, just vote for it */
	if (val & PLL_VOTE_FSM_ENA) {
		ret = clk_enable_regmap(hw);
		if (ret)
			return ret;
		return wait_for_pll_enable_active(pll);
	}

	/* Skip if already enabled */
	if ((val & mask) == mask)
		return 0;

	ret = regmap_update_bits(pll->clkr.regmap, off,
				 PLL_BYPASSNL, PLL_BYPASSNL);
	if (ret)
		return ret;

	/*
	 * H/W requires a 5us delay between disabling the bypass and
	 * de-asserting the reset.
	 */
	mb();
	udelay(5);

	ret = regmap_update_bits(pll->clkr.regmap, off,
				 PLL_RESET_N, PLL_RESET_N);
	if (ret)
		return ret;

	ret = wait_for_pll_enable_lock(pll);
	if (ret)
		return ret;

	ret = regmap_update_bits(pll->clkr.regmap, off,
				 PLL_OUTCTRL, PLL_OUTCTRL);

	/* Ensure that the write above goes through before returning. */
	mb();
	return ret;
}

static void alpha_pll_default_disable(struct clk_hw *hw)
{
	int ret;
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	u32 val, mask, off = pll->offset;

	ret = regmap_read(pll->clkr.regmap, off, &val);
	if (ret)
		return;

	/* If in FSM mode, just unvote it */
	if (val & PLL_VOTE_FSM_ENA) {
		clk_disable_regmap(hw);
		return;
	}

	mask = PLL_OUTCTRL;
	regmap_update_bits(pll->clkr.regmap, off, mask, 0);

	/* Delay of 2 output clock ticks required until output is disabled */
	mb();
	udelay(1);

	mask = PLL_RESET_N | PLL_BYPASSNL;
	regmap_update_bits(pll->clkr.regmap, off, mask, 0);
}

static unsigned long alpha_pll_calc_rate(u64 prate, u32 l, u32 a)
{
	return (prate * l) + ((prate * a) >> ALPHA_BITWIDTH);
}

static unsigned long
alpha_pll_round_rate(unsigned long rate, unsigned long prate, u32 *l, u64 *a)
{
	u64 remainder;
	u64 quotient;

	quotient = rate;
	remainder = do_div(quotient, prate);
	*l = quotient;

	if (!remainder) {
		*a = 0;
		return rate;
	}

	/* Upper ALPHA_BITWIDTH bits of Alpha */
	quotient = remainder << ALPHA_BITWIDTH;
	remainder = do_div(quotient, prate);

	if (remainder)
		quotient++;

	*a = quotient;
	return alpha_pll_calc_rate(prate, *l, *a);
}

static const struct pll_vco *
alpha_pll_find_vco(const struct clk_alpha_pll *pll, unsigned long rate)
{
	const struct pll_vco *v = pll->vco_table;
	const struct pll_vco *end = v + pll->num_vco;

	for (; v < end; v++)
		if (rate >= v->min_freq && rate <= v->max_freq)
			return v;

	return NULL;
}

static unsigned long
alpha_pll_default_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	u32 l, low, high, ctl;
	u64 a = 0, prate = parent_rate;
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	u32 off = pll->offset;
	u8 type = pll->pll_type;

	regmap_read(pll->clkr.regmap, off + pll_l(type), &l);

	regmap_read(pll->clkr.regmap, off + pll_user_ctl(type), &ctl);
	if (ctl & PLL_ALPHA_EN) {
		regmap_read(pll->clkr.regmap, off + pll_alpha(type), &low);
		if (pll->flags & SUPPORTS_16BIT_ALPHA) {
			a = low & ALPHA_16BIT_MASK;
		} else {
			regmap_read(pll->clkr.regmap, off + pll_alpha_u(type),
				    &high);
			a = (u64)high << 32 | low;
			a >>= ALPHA_REG_BITWIDTH - ALPHA_BITWIDTH;
		}
	}

	return alpha_pll_calc_rate(prate, l, a);
}

static int alpha_pll_default_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long prate)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	const struct pll_vco *vco;
	u32 l, off = pll->offset;
	u8 type = pll->pll_type;
	u64 a;

	rate = alpha_pll_round_rate(rate, prate, &l, &a);
	vco = alpha_pll_find_vco(pll, rate);
	if (!vco) {
		pr_err("alpha pll not in a valid vco range\n");
		return -EINVAL;
	}

	regmap_write(pll->clkr.regmap, off + pll_l(type), l);

	if (pll->flags & SUPPORTS_16BIT_ALPHA) {
		regmap_write(pll->clkr.regmap, off + pll_alpha(type),
			     a & ALPHA_16BIT_MASK);
	} else {
		a <<= (ALPHA_REG_BITWIDTH - ALPHA_BITWIDTH);
		regmap_write(pll->clkr.regmap, off + pll_alpha_u(type),
			     a >> 32);
	}

	regmap_update_bits(pll->clkr.regmap, off + pll_user_ctl(type),
			   PLL_VCO_MASK << PLL_VCO_SHIFT,
			   vco->val << PLL_VCO_SHIFT);

	regmap_update_bits(pll->clkr.regmap, off + pll_user_ctl(type),
			   PLL_ALPHA_EN, PLL_ALPHA_EN);

	return 0;
}

static long alpha_pll_default_round_rate(struct clk_hw *hw, unsigned long rate,
					 unsigned long *prate)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	u32 l;
	u64 a;
	unsigned long min_freq, max_freq;

	rate = alpha_pll_round_rate(rate, *prate, &l, &a);
	if (alpha_pll_find_vco(pll, rate))
		return rate;

	min_freq = pll->vco_table[0].min_freq;
	max_freq = pll->vco_table[pll->num_vco - 1].max_freq;

	return clamp(rate, min_freq, max_freq);
}

static int clk_alpha_pll_enable(struct clk_hw *hw)
{
	return pll_clk_ops(hw).enable(hw);
}

static void clk_alpha_pll_disable(struct clk_hw *hw)
{
	pll_clk_ops(hw).disable(hw);
}

static int clk_alpha_pll_is_enabled(struct clk_hw *hw)
{
	return pll_clk_ops(hw).is_enabled(hw);
}

static unsigned long
clk_alpha_pll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	return pll_clk_ops(hw).recalc_rate(hw, parent_rate);
}

static long clk_alpha_pll_round_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long *prate)
{
	return pll_clk_ops(hw).round_rate(hw, rate, prate);
}

static int clk_alpha_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long prate)
{
	return pll_clk_ops(hw).set_rate(hw, rate, prate);
}

static int clk_alpha_pll_hwfsm_enable(struct clk_hw *hw)
{
	return pll_clk_ops(hw).hwfsm_enable(hw);
}

static void clk_alpha_pll_hwfsm_disable(struct clk_hw *hw)
{
	pll_clk_ops(hw).hwfsm_disable(hw);
}

static int clk_alpha_pll_hwfsm_is_enabled(struct clk_hw *hw)
{
	return pll_clk_ops(hw).hwfsm_is_enabled(hw);
}

const struct clk_ops clk_alpha_pll_ops = {
	.enable = clk_alpha_pll_enable,
	.disable = clk_alpha_pll_disable,
	.is_enabled = clk_alpha_pll_is_enabled,
	.recalc_rate = clk_alpha_pll_recalc_rate,
	.round_rate = clk_alpha_pll_round_rate,
	.set_rate = clk_alpha_pll_set_rate,
};
EXPORT_SYMBOL_GPL(clk_alpha_pll_ops);

const struct clk_ops clk_alpha_pll_hwfsm_ops = {
	.enable = clk_alpha_pll_hwfsm_enable,
	.disable = clk_alpha_pll_hwfsm_disable,
	.is_enabled = clk_alpha_pll_hwfsm_is_enabled,
	.recalc_rate = clk_alpha_pll_recalc_rate,
	.round_rate = clk_alpha_pll_round_rate,
	.set_rate = clk_alpha_pll_set_rate,
};
EXPORT_SYMBOL_GPL(clk_alpha_pll_hwfsm_ops);

static unsigned long
clk_alpha_pll_postdiv_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct clk_alpha_pll_postdiv *pll = to_clk_alpha_pll_postdiv(hw);
	u32 ctl;

	regmap_read(pll->clkr.regmap, pll->offset + pll_user_ctl(pll->pll_type),
		    &ctl);

	ctl >>= PLL_POST_DIV_SHIFT;
	ctl &= PLL_POST_DIV_MASK;

	return parent_rate >> fls(ctl);
}

static const struct clk_div_table clk_alpha_div_table[] = {
	{ 0x0, 1 },
	{ 0x1, 2 },
	{ 0x3, 4 },
	{ 0x7, 8 },
	{ 0xf, 16 },
	{ }
};

static long
clk_alpha_pll_postdiv_round_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long *prate)
{
	struct clk_alpha_pll_postdiv *pll = to_clk_alpha_pll_postdiv(hw);

	return divider_round_rate(hw, rate, prate, clk_alpha_div_table,
				  pll->width, CLK_DIVIDER_POWER_OF_TWO);
}

static int clk_alpha_pll_postdiv_set_rate(struct clk_hw *hw, unsigned long rate,
					  unsigned long parent_rate)
{
	struct clk_alpha_pll_postdiv *pll = to_clk_alpha_pll_postdiv(hw);
	int div;

	/* 16 -> 0xf, 8 -> 0x7, 4 -> 0x3, 2 -> 0x1, 1 -> 0x0 */
	div = DIV_ROUND_UP_ULL((u64)parent_rate, rate) - 1;

	return regmap_update_bits(pll->clkr.regmap, pll->offset +
				  pll_user_ctl(pll->pll_type),
				  PLL_POST_DIV_MASK << PLL_POST_DIV_SHIFT,
				  div << PLL_POST_DIV_SHIFT);
}

const struct clk_ops clk_alpha_pll_postdiv_ops = {
	.recalc_rate = clk_alpha_pll_postdiv_recalc_rate,
	.round_rate = clk_alpha_pll_postdiv_round_rate,
	.set_rate = clk_alpha_pll_postdiv_set_rate,
};
EXPORT_SYMBOL_GPL(clk_alpha_pll_postdiv_ops);

/* Contains actual property values for different PLL types */
static const struct
alpha_pll_props alpha_pll_props[CLK_ALPHA_PLL_TYPE_MAX] = {
	[CLK_ALPHA_PLL_TYPE_DEFAULT] =  {
		.reg_offsets = {
			[PLL_L_VAL] = 0x04,
			[PLL_ALPHA_VAL] = 0x08,
			[PLL_ALPHA_VAL_U] = 0x0c,
			[PLL_USER_CTL] = 0x10,
			[PLL_USER_CTL_U] = 0x14,
			[PLL_CONFIG_CTL] = 0x18,
			[PLL_TEST_CTL] = 0x1c,
			[PLL_TEST_CTL_U] = 0x20,
			[PLL_STATUS] = 0x24,
		},
		.ops = {
			.enable = alpha_pll_default_enable,
			.disable = alpha_pll_default_disable,
			.is_enabled = alpha_pll_default_is_enabled,
			.hwfsm_enable = alpha_pll_default_hwfsm_enable,
			.hwfsm_disable = alpha_pll_default_hwfsm_disable,
			.hwfsm_is_enabled = alpha_pll_default_hwfsm_is_enabled,
			.recalc_rate = alpha_pll_default_recalc_rate,
			.round_rate = alpha_pll_default_round_rate,
			.set_rate = alpha_pll_default_set_rate,
		},
	},
};
