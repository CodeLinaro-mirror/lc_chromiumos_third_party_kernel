// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/bitops.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/core.h>
#include <linux/mfd/qcom_pm8008.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <dt-bindings/mfd/qcom-pm8008.h>

#define I2C_INTR_STATUS_BASE		0x0550
#define INT_RT_STS_OFFSET		0x10
#define INT_SET_TYPE_OFFSET		0x11
#define INT_POL_HIGH_OFFSET		0x12
#define INT_POL_LOW_OFFSET		0x13
#define INT_LATCHED_CLR_OFFSET		0x14
#define INT_EN_SET_OFFSET		0x15
#define INT_EN_CLR_OFFSET		0x16
#define INT_LATCHED_STS_OFFSET		0x18

static const struct mfd_cell pm8008_regulator_devs[] = {
	MFD_CELL_NAME("qcom-pm8008-regulator"),
};

enum {
	PM8008_MISC,
	PM8008_TEMP_ALARM,
	PM8008_GPIO1,
	PM8008_GPIO2,
	PM8008_NUM_PERIPHS,
};

#define PM8008_PERIPH_0_BASE	0x900
#define PM8008_PERIPH_1_BASE	0x2400
#define PM8008_PERIPH_2_BASE	0xC000
#define PM8008_PERIPH_3_BASE	0xC100

#define PM8008_TEMP_ALARM_ADDR	PM8008_PERIPH_1_BASE
#define PM8008_GPIO1_ADDR	PM8008_PERIPH_2_BASE
#define PM8008_GPIO2_ADDR	PM8008_PERIPH_3_BASE

#define PM8008_STATUS_BASE	(PM8008_PERIPH_0_BASE | INT_LATCHED_STS_OFFSET)
#define PM8008_MASK_BASE	(PM8008_PERIPH_0_BASE | INT_EN_SET_OFFSET)
#define PM8008_UNMASK_BASE	(PM8008_PERIPH_0_BASE | INT_EN_CLR_OFFSET)
#define PM8008_TYPE_BASE	(PM8008_PERIPH_0_BASE | INT_SET_TYPE_OFFSET)
#define PM8008_ACK_BASE		(PM8008_PERIPH_0_BASE | INT_LATCHED_CLR_OFFSET)
#define PM8008_POLARITY_HI_BASE	(PM8008_PERIPH_0_BASE | INT_POL_HIGH_OFFSET)
#define PM8008_POLARITY_LO_BASE	(PM8008_PERIPH_0_BASE | INT_POL_LOW_OFFSET)

#define PM8008_PERIPH_OFFSET(paddr)	(paddr - PM8008_PERIPH_0_BASE)

struct pm8008_data {
	bool ready;
	struct device *dev;
	struct i2c_client *clients[PM8008_NUM_CLIENTS];
	struct regmap *regmap[PM8008_NUM_CLIENTS];
	struct gpio_desc *reset_gpio;
	int irq;
	struct regmap_irq_chip_data *irq_data;
};

static unsigned int p0_offs[] = {PM8008_PERIPH_OFFSET(PM8008_PERIPH_0_BASE)};
static unsigned int p1_offs[] = {PM8008_PERIPH_OFFSET(PM8008_PERIPH_1_BASE)};
static unsigned int p2_offs[] = {PM8008_PERIPH_OFFSET(PM8008_PERIPH_2_BASE)};
static unsigned int p3_offs[] = {PM8008_PERIPH_OFFSET(PM8008_PERIPH_3_BASE)};

static struct regmap_irq_sub_irq_map pm8008_sub_reg_offsets[] = {
	REGMAP_IRQ_MAIN_REG_OFFSET(p0_offs),
	REGMAP_IRQ_MAIN_REG_OFFSET(p1_offs),
	REGMAP_IRQ_MAIN_REG_OFFSET(p2_offs),
	REGMAP_IRQ_MAIN_REG_OFFSET(p3_offs),
};

static unsigned int pm8008_virt_regs[] = {
	PM8008_POLARITY_HI_BASE,
	PM8008_POLARITY_LO_BASE,
};

enum {
	POLARITY_HI_INDEX,
	POLARITY_LO_INDEX,
	PM8008_NUM_VIRT_REGS,
};

static struct regmap_irq pm8008_irqs[] = {
	REGMAP_IRQ_REG(PM8008_IRQ_MISC_UVLO,	PM8008_MISC,	BIT(0)),
	REGMAP_IRQ_REG(PM8008_IRQ_MISC_OVLO,	PM8008_MISC,	BIT(1)),
	REGMAP_IRQ_REG(PM8008_IRQ_MISC_OTST2,	PM8008_MISC,	BIT(2)),
	REGMAP_IRQ_REG(PM8008_IRQ_MISC_OTST3,	PM8008_MISC,	BIT(3)),
	REGMAP_IRQ_REG(PM8008_IRQ_MISC_LDO_OCP,	PM8008_MISC,	BIT(4)),
	REGMAP_IRQ_REG(PM8008_IRQ_TEMP_ALARM,	PM8008_TEMP_ALARM, BIT(0)),
	REGMAP_IRQ_REG(PM8008_IRQ_GPIO1,	PM8008_GPIO1,	BIT(0)),
	REGMAP_IRQ_REG(PM8008_IRQ_GPIO2,	PM8008_GPIO2,	BIT(0)),
};

static int pm8008_set_type_virt(unsigned int **virt_buf,
				      unsigned int type, unsigned long hwirq,
				      int reg)
{
	switch (type) {
	case IRQ_TYPE_EDGE_FALLING:
	case IRQ_TYPE_LEVEL_LOW:
		virt_buf[POLARITY_HI_INDEX][reg] &= ~pm8008_irqs[hwirq].mask;
		virt_buf[POLARITY_LO_INDEX][reg] |= pm8008_irqs[hwirq].mask;
		break;

	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_LEVEL_HIGH:
		virt_buf[POLARITY_HI_INDEX][reg] |= pm8008_irqs[hwirq].mask;
		virt_buf[POLARITY_LO_INDEX][reg] &= ~pm8008_irqs[hwirq].mask;
		break;

	case IRQ_TYPE_EDGE_BOTH:
		virt_buf[POLARITY_HI_INDEX][reg] |= pm8008_irqs[hwirq].mask;
		virt_buf[POLARITY_LO_INDEX][reg] |= pm8008_irqs[hwirq].mask;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static struct regmap_irq_chip pm8008_irq_chip = {
	.name			= "pm8008_irq",
	.main_status		= I2C_INTR_STATUS_BASE,
	.num_main_regs		= 1,
	.num_virt_regs		= PM8008_NUM_VIRT_REGS,
	.irqs			= pm8008_irqs,
	.num_irqs		= ARRAY_SIZE(pm8008_irqs),
	.num_regs		= PM8008_NUM_PERIPHS,
	.not_fixed_stride	= true,
	.sub_reg_offsets	= pm8008_sub_reg_offsets,
	.set_type_virt		= pm8008_set_type_virt,
	.status_base		= PM8008_STATUS_BASE,
	.mask_base		= PM8008_MASK_BASE,
	.unmask_base		= PM8008_UNMASK_BASE,
	.type_base		= PM8008_TYPE_BASE,
	.ack_base		= PM8008_ACK_BASE,
	.virt_reg_base		= pm8008_virt_regs,
	.num_type_reg		= PM8008_NUM_PERIPHS,
};

static struct regmap_config qcom_mfd_regmap_cfg = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= 0xFFFF,
};

struct regmap *pm8008_get_regmap(struct pm8008_data *chip, u8 sid)
{
	if (!chip || !chip->ready) {
		pr_err("pm8008 chip not initialized\n");
		return NULL;
	}

	return chip->regmap[sid];
}

static int pm8008_init(struct pm8008_data *chip)
{
	int rc;
	struct regmap *regmap = pm8008_get_regmap(chip, PM8008_INFRA_SID);

	/*
	 * Set TEMP_ALARM peripheral's TYPE so that the regmap-irq framework
	 * reads this as the default value instead of zero, the HW default.
	 * This is required to enable the writing of TYPE registers in
	 * regmap_irq_sync_unlock().
	 */
	rc = regmap_write(regmap,
			 (PM8008_TEMP_ALARM_ADDR | INT_SET_TYPE_OFFSET),
			 BIT(0));
	if (rc)
		return rc;

	/* Do the same for GPIO1 and GPIO2 peripherals */
	rc = regmap_write(regmap,
			 (PM8008_GPIO1_ADDR | INT_SET_TYPE_OFFSET), BIT(0));
	if (rc)
		return rc;

	rc = regmap_write(regmap,
			 (PM8008_GPIO2_ADDR | INT_SET_TYPE_OFFSET), BIT(0));

	return rc;
}

static int pm8008_probe_irq_peripherals(struct pm8008_data *chip,
					int client_irq)
{
	int rc, i;
	struct regmap_irq_type *type;
	struct regmap_irq_chip_data *irq_data;
	struct regmap *regmap = pm8008_get_regmap(chip, PM8008_INFRA_SID);

	rc = pm8008_init(chip);
	if (rc) {
		dev_err(chip->dev, "Init failed: %d\n", rc);
		return rc;
	}

	for (i = 0; i < ARRAY_SIZE(pm8008_irqs); i++) {
		type = &pm8008_irqs[i].type;

		type->type_reg_offset	  = pm8008_irqs[i].reg_offset;
		type->type_rising_val	  = pm8008_irqs[i].mask;
		type->type_falling_val	  = pm8008_irqs[i].mask;
		type->type_level_high_val = 0;
		type->type_level_low_val  = 0;

		if (type->type_reg_offset == PM8008_MISC)
			type->types_supported = IRQ_TYPE_EDGE_RISING;
		else
			type->types_supported = (IRQ_TYPE_EDGE_BOTH |
				IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW);
	}

	rc = devm_regmap_add_irq_chip(chip->dev, regmap, client_irq,
			IRQF_SHARED, 0, &pm8008_irq_chip, &irq_data);
	if (rc) {
		dev_err(chip->dev, "Failed to add IRQ chip: %d\n", rc);
		return rc;
	}

	return 0;
}

static int pm8008_probe(struct i2c_client *client)
{
	int rc, i;
	struct pm8008_data *chip;
	struct device_node *node = client->dev.of_node;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;

	for (i = 0; i < PM8008_NUM_CLIENTS; i++) {
		if (i == 0) {
			chip->clients[i] = client;
		} else {
			chip->clients[i] = i2c_new_dummy_device(client->adapter,
								client->addr + i);
			if (IS_ERR(chip->clients[i])) {
				dev_err(&client->dev, "can't attach client %d\n", i);
				return PTR_ERR(chip->clients[i]);
			}
			chip->clients[i]->dev.of_node = of_node_get(node);
		}

		chip->regmap[i] = devm_regmap_init_i2c(chip->clients[i],
							&qcom_mfd_regmap_cfg);
		if (!chip->regmap[i])
			return -ENODEV;

		i2c_set_clientdata(chip->clients[i], chip);
	}

	chip->ready = true;

	if (of_property_read_bool(chip->dev->of_node, "interrupt-controller")) {
		rc = pm8008_probe_irq_peripherals(chip, client->irq);
		if (rc)
			dev_err(chip->dev, "Failed to probe irq periphs: %d\n", rc);
	}

	chip->reset_gpio = devm_gpiod_get(chip->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(chip->reset_gpio)) {
		dev_err(chip->dev, "failed to acquire reset gpio\n");
		return PTR_ERR(chip->reset_gpio);
	}
	gpiod_set_value(chip->reset_gpio, 1);

	rc = devm_mfd_add_devices(&chip->clients[PM8008_REGULATORS_SID]->dev,
			0, pm8008_regulator_devs, ARRAY_SIZE(pm8008_regulator_devs),
			NULL, 0, NULL);
	if (rc) {
		dev_err(chip->dev, "Failed to add regulators: %d\n", rc);
		return rc;
	}

	return devm_of_platform_populate(chip->dev);
}

static const struct of_device_id pm8008_match[] = {
	{ .compatible = "qcom,pm8008", },
	{ },
};

static struct i2c_driver pm8008_mfd_driver = {
	.driver = {
		.name = "pm8008",
		.of_match_table = pm8008_match,
	},
	.probe_new = pm8008_probe,
};
module_i2c_driver(pm8008_mfd_driver);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("i2c:qcom-pm8008");
