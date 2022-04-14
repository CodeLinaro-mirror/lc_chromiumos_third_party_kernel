/* Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOM_PM8008_H__
#define __QCOM_PM8008_H__

#define PM8008_INFRA_SID	0
#define PM8008_REGULATORS_SID	1

#define PM8008_NUM_CLIENTS	2

struct pm8008_data;
struct regmap *pm8008_get_regmap(struct pm8008_data *chip, u8 sid);

#endif
