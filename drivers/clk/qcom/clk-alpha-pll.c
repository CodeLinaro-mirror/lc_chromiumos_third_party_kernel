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
# define PLL_UPDATE		BIT(22)
# define PLL_UPDATE_BYPASS	BIT(23)
# define PLL_OFFLINE_ACK	BIT(28)
# define ALPHA_PLL_ACK_LATCH	BIT(29)
# define PLL_ACTIVE_FLAG	BIT(30)
# define PLL_LOCK_DET		BIT(31)

# define PLL_POST_DIV_SHIFT	8
# define PLL_POST_DIV_MASK	0xf
# define PLL_ALPHA_EN		BIT(24)
# define PLL_ALPHA_MODE		BIT(25)
# define PLL_VCO_SHIFT		20
# define PLL_VCO_MASK		0x3

#define PLL_HUAYRA_M_WIDTH		8
#define PLL_HUAYRA_M_SHIFT		8
#define PLL_HUAYRA_M_MASK		0xff
#define PLL_HUAYRA_N_SHIFT		0
#define PLL_HUAYRA_N_MASK		0xff
#define PLL_HUAYRA_ALPHA_WIDTH		16

/*
 * Even though 40 bits are present, use only 32 for ease of calculation.
 */
#define ALPHA_BITWIDTH		32

/* Returns the Alpha register width for pll type */
#define pll_alpha_width(type)	(alpha_pll_props[type].alpha_width)

/* Returns the flags for pll type */
#define pll_flags(type)		(alpha_pll_props[type].flags)

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
 * @alpha_width: alpha value width
 * @ops: clock operations for alpha PLL
 */
struct alpha_pll_props {
	u8 reg_offsets[PLL_MAX_REGS];
	u8 alpha_width;

#define HAVE_64BIT_CONFIG_CTL		BIT(0)
#define SUPPORTS_DYNAMIC_UPDATE		BIT(1)
#define SUPPORTS_VCO			BIT(2)
	u8 flags;
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

#define wait_for_pll_update(pll) \
	wait_for_pll(pll, PLL_UPDATE, 1, "update")

#define wait_for_pll_update_ack_set(pll) \
	wait_for_pll(pll, ALPHA_PLL_ACK_LATCH, 0, "update_ack_set")

#define wait_for_pll_update_ack_clear(pll) \
	wait_for_pll(pll, ALPHA_PLL_ACK_LATCH, 1, "update_ack_clear")

void clk_alpha_pll_configure(struct clk_alpha_pll *pll, struct regmap *regmap,
			     const struct alpha_pll_config *config)
{
	u32 val, mask;
	u32 off = pll->offset;
	u8 type = pll->pll_type, flags = pll_flags(type);

	regmap_write(regmap, off + pll_l(type), config->l);
	regmap_write(regmap, off + pll_alpha(type), config->alpha);
	regmap_write(regmap, off + pll_cfg_ctl(type), config->config_ctl_val);
	regmap_write(regmap, off + pll_cfg_ctl_u(type),
		     config->config_ctl_hi_val);

	if (flags & HAVE_64BIT_CONFIG_CTL)
		regmap_write(regmap, off + pll_cfg_ctl_u(type),
			     config->config_ctl_hi_val);

	if (pll_alpha_width(type) > 32)
		regmap_write(regmap, off + pll_alpha_u(type),
			     config->alpha_hi);

	val = config->main_output_mask;
	val |= config->aux_output_mask;
	val |= config->aux2_output_mask;
	val |= config->early_output_mask;
	val |= config->pre_div_val;
	val |= config->post_div_val;
	val |= config->vco_val;
	val |= config->alpha_en_mask;
	val |= config->alpha_mode_mask;

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

static unsigned long
alpha_pll_calc_rate(u64 prate, u32 l, u32 a, u32 alpha_width)
{
	return (prate * l) + ((prate * a) >>
		(alpha_width < ALPHA_BITWIDTH ? alpha_width : ALPHA_BITWIDTH));
}

static unsigned long
alpha_pll_round_rate(unsigned long rate, unsigned long prate, u32 *l, u64 *a,
		     u32 alpha_width)
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
	quotient = remainder << (alpha_width < ALPHA_BITWIDTH ?
				 alpha_width : ALPHA_BITWIDTH);

	remainder = do_div(quotient, prate);

	if (remainder)
		quotient++;

	*a = quotient;
	return alpha_pll_calc_rate(prate, *l, *a, alpha_width);
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
	u8 type = pll->pll_type;
	u32 off = pll->offset, alpha_width = pll_alpha_width(type);

	regmap_read(pll->clkr.regmap, off + pll_l(type), &l);

	regmap_read(pll->clkr.regmap, off + pll_user_ctl(type), &ctl);
	if (ctl & PLL_ALPHA_EN) {
		regmap_read(pll->clkr.regmap, off + pll_alpha(type), &low);
		if (alpha_width > 32) {
			regmap_read(pll->clkr.regmap, off + pll_alpha_u(type),
				    &high);
			a = (u64)high << 32 | low;
		} else {
			a = low & GENMASK(alpha_width - 1, 0);
		}

		if (alpha_width > ALPHA_BITWIDTH)
			a >>= alpha_width - ALPHA_BITWIDTH;
	}

	return alpha_pll_calc_rate(prate, l, a, alpha_width);
}

static int clk_alpha_pll_update_latch(struct clk_alpha_pll *pll)
{
	int ret;
	u32 mode, off = pll->offset;

	regmap_read(pll->clkr.regmap, off, &mode);

	/* Latch the input to the PLL */
	regmap_update_bits(pll->clkr.regmap, off, PLL_UPDATE, PLL_UPDATE);

	/* Make sure PLL_UPDATE request goes through*/
	mb();

	/* Wait for 2 reference cycle before checking ACK bit */
	udelay(1);

	/*
	 * PLL will latch the new L, Alpha and freq control word.
	 * PLL will respond by raising PLL_ACK_LATCH output when new programming
	 * has been latched in and PLL is being updated. When
	 * UPDATE_LOGIC_BYPASS bit is not set, PLL_UPDATE will be cleared
	 * automatically by hardware when PLL_ACK_LATCH is asserted by PLL.
	 */
	if (mode & PLL_UPDATE_BYPASS) {
		ret = wait_for_pll_update_ack_set(pll);
		if (ret)
			return ret;

		regmap_update_bits(pll->clkr.regmap, off, PLL_UPDATE, 0);

		/* Make sure PLL_UPDATE request goes through*/
		mb();
	} else {
		ret = wait_for_pll_update(pll);
		if (ret)
			return ret;
	}

	ret = wait_for_pll_update_ack_clear(pll);
	if (ret)
		return ret;

	/* Wait for PLL output to stabilize */
	udelay(10);

	return 0;
}

static int alpha_pll_default_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long prate)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	const struct pll_vco *vco;
	u8 type = pll->pll_type, flags = pll_flags(type);
	u32 l, off = pll->offset, alpha_width = pll_alpha_width(type);
	u64 a;

	rate = alpha_pll_round_rate(rate, prate, &l, &a, alpha_width);
	if (flags & SUPPORTS_VCO) {
		vco = alpha_pll_find_vco(pll, rate);
		if (!vco) {
			pr_err("alpha pll not in a valid vco range\n");
			return -EINVAL;
		}
	}

	regmap_write(pll->clkr.regmap, off + pll_l(type), l);

	if (alpha_width > ALPHA_BITWIDTH)
		a <<= alpha_width - ALPHA_BITWIDTH;

	if (alpha_width > 32)
		regmap_write(pll->clkr.regmap, off + pll_alpha_u(type),
			     a >> 32);

	regmap_write(pll->clkr.regmap, off + pll_alpha(type), a);

	if (flags & SUPPORTS_VCO)
		regmap_update_bits(pll->clkr.regmap, off + pll_user_ctl(type),
				   PLL_VCO_MASK << PLL_VCO_SHIFT,
				   vco->val << PLL_VCO_SHIFT);

	regmap_update_bits(pll->clkr.regmap, off + pll_user_ctl(type),
			   PLL_ALPHA_EN, PLL_ALPHA_EN);

	if (!clk_hw_is_enabled(hw) || !(flags & SUPPORTS_DYNAMIC_UPDATE))
		return 0;

	return clk_alpha_pll_update_latch(pll);
}

static long alpha_pll_default_round_rate(struct clk_hw *hw, unsigned long rate,
					 unsigned long *prate)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	u8 type = pll->pll_type;
	u32 l, alpha_width = pll_alpha_width(type);
	u64 a;
	unsigned long min_freq, max_freq;

	rate = alpha_pll_round_rate(rate, *prate, &l, &a, alpha_width);
	if (!(pll_flags(type) & SUPPORTS_VCO) || alpha_pll_find_vco(pll, rate))
		return rate;

	min_freq = pll->vco_table[0].min_freq;
	max_freq = pll->vco_table[pll->num_vco - 1].max_freq;

	return clamp(rate, min_freq, max_freq);
}

static unsigned long
alpha_huayra_pll_calc_rate(u64 prate, u32 l, u32 a)
{
	/*
	 * a contains 16 bit alpha_val in two’s compliment number in the range
	 * of [-0.5, 0.5).
	 */
	if (a >= BIT(PLL_HUAYRA_ALPHA_WIDTH - 1))
		l -= 1;

	return (prate * l) + (prate * a >> PLL_HUAYRA_ALPHA_WIDTH);
}

static unsigned long
alpha_huayra_pll_round_rate(unsigned long rate, unsigned long prate,
			    u32 *l, u32 *a)
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

	quotient = remainder << PLL_HUAYRA_ALPHA_WIDTH;
	remainder = do_div(quotient, prate);

	if (remainder)
		quotient++;

	/*
	 * alpha_val should be in two’s compliment number in the range
	 * of [-0.5, 0.5) so if quotient >= 0.5 then increment the l value
	 * since alpha value will be subtracted in this case.
	 */
	if (quotient >= BIT(PLL_HUAYRA_ALPHA_WIDTH - 1))
		*l += 1;

	*a = quotient;
	return alpha_huayra_pll_calc_rate(prate, *l, *a);
}

static unsigned long
alpha_pll_huayra_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	u64 rate = parent_rate, tmp;
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	u8 type = pll->pll_type;
	u32 l, alpha = 0, ctl, alpha_m, alpha_n, off = pll->offset;

	regmap_read(pll->clkr.regmap, off + pll_l(type), &l);
	regmap_read(pll->clkr.regmap, off + pll_user_ctl(type), &ctl);

	if (ctl & PLL_ALPHA_EN) {
		regmap_read(pll->clkr.regmap, off + pll_alpha(type), &alpha);
		/*
		 * Depending upon alpha_mode, it can be treated as M/N value or
		 * as a two’s compliment number. When
		 * alpha_mode=1 pll_alpha_val<15:8>=M & pll_apla_val<7:0>=N
		 *		Fout=FIN*(L+(M/N))
		 * M is a signed number (-128 to 127) and N is unsigned
		 * (0 to 255). M/N has to be within +/-0.5.
		 *
		 * alpha_mode=0, it is a two’s compliment number in the range
		 * of [-0.5, 0.5).
		 *		Fout=FIN*(L+(alpha_val)/2^16),where alpha_val is
		 * two’s compliment number.
		 */
		if (!(ctl & PLL_ALPHA_MODE))
			return alpha_huayra_pll_calc_rate(rate, l, alpha);

		alpha_m = alpha >> PLL_HUAYRA_M_SHIFT & PLL_HUAYRA_M_MASK;
		alpha_n = alpha >> PLL_HUAYRA_N_SHIFT & PLL_HUAYRA_N_MASK;

		rate *= l;
		tmp = parent_rate;
		if (alpha_m >= BIT(PLL_HUAYRA_M_WIDTH - 1)) {
			alpha_m = BIT(PLL_HUAYRA_M_WIDTH) - alpha_m;
			tmp *= alpha_m;
			do_div(tmp, alpha_n);
			rate -= tmp;
		} else {
			tmp *= alpha_m;
			do_div(tmp, alpha_n);
			rate += tmp;
		}

		return rate;
	}

	return alpha_huayra_pll_calc_rate(rate, l, alpha);
}

static int alpha_pll_huayra_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long prate)
{
	struct clk_alpha_pll *pll = to_clk_alpha_pll(hw);
	u8 type = pll->pll_type;
	u32 l, a, ctl, cur_alpha = 0, off = pll->offset;

	rate = alpha_huayra_pll_round_rate(rate, prate, &l, &a);

	regmap_read(pll->clkr.regmap, off + pll_user_ctl(type), &ctl);

	if (ctl & PLL_ALPHA_EN)
		regmap_read(pll->clkr.regmap, off + pll_alpha(type),
			    &cur_alpha);

	/*
	 * Huayra PLL supports PLL dynamic programming. User can change L_VAL,
	 * without having to go through the power on sequence.
	 */
	if (clk_hw_is_enabled(hw)) {
		if (cur_alpha != a) {
			pr_err("clock needs to be gated %s\n",
			       clk_hw_get_name(hw));
			return -EBUSY;
		}

		regmap_write(pll->clkr.regmap, off + pll_l(type), l);
		/* Ensure that the write above goes to detect L val change. */
		mb();
		return wait_for_pll_enable_lock(pll);
	}

	regmap_write(pll->clkr.regmap, off + pll_l(type), l);
	regmap_write(pll->clkr.regmap, off + pll_alpha(type), a);

	if (a == 0)
		regmap_update_bits(pll->clkr.regmap, off + pll_user_ctl(type),
				   PLL_ALPHA_EN, 0x0);
	else
		regmap_update_bits(pll->clkr.regmap, off + pll_user_ctl(type),
				   PLL_ALPHA_EN | PLL_ALPHA_MODE, PLL_ALPHA_EN);

	return 0;
}

static long alpha_pll_huayra_round_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long *prate)
{
	u32 l, a;

	return alpha_huayra_pll_round_rate(rate, *prate, &l, &a);
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
		.alpha_width = 40,
		.flags = SUPPORTS_VCO,
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
	[CLK_ALPHA_PLL_TYPE_HUAYRA] =  {
		.reg_offsets = {
			[PLL_L_VAL] = 0x04,
			[PLL_ALPHA_VAL] = 0x08,
			[PLL_USER_CTL] = 0x10,
			[PLL_CONFIG_CTL] = 0x14,
			[PLL_CONFIG_CTL_U] = 0x18,
			[PLL_TEST_CTL] = 0x1c,
			[PLL_TEST_CTL_U] = 0x20,
			[PLL_STATUS] = 0x24,
		},
		.alpha_width = 16,
		.flags = SUPPORTS_DYNAMIC_UPDATE | HAVE_64BIT_CONFIG_CTL,
		.ops = {
			.enable = alpha_pll_default_enable,
			.disable = alpha_pll_default_disable,
			.is_enabled = alpha_pll_default_is_enabled,
			.hwfsm_enable = alpha_pll_default_hwfsm_enable,
			.hwfsm_disable = alpha_pll_default_hwfsm_disable,
			.hwfsm_is_enabled = alpha_pll_default_hwfsm_is_enabled,
			.recalc_rate = alpha_pll_huayra_recalc_rate,
			.round_rate = alpha_pll_huayra_round_rate,
			.set_rate = alpha_pll_huayra_set_rate,
		},
	},
};
