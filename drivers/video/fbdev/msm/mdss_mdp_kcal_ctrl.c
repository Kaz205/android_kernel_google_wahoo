/*
 * Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 * Copyright (c) 2013, LGE Inc. All rights reserved.
 * Copyright (c) 2014, Savoca <adeddo27@gmail.com>
 * Copyright (c) 2017-2020, Alex Saiko <solcmdr@gmail.com>
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

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#define KCAL_CTRL	"kcal_ctrl"

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "mdss_fb.h"
#include "mdss_mdp.h"

#define PCC_ADJ				(128)
#define MDSS_MDP_KCAL_ENABLED		(1)
#define MDSS_MDP_KCAL_MIN_VALUE		(35)
#define MDSS_MDP_KCAL_INIT_RED		(256)
#define MDSS_MDP_KCAL_INIT_GREEN	(256)
#define MDSS_MDP_KCAL_INIT_BLUE		(256)
#define MDSS_MDP_KCAL_INIT_HUE		(0)
#define MDSS_MDP_KCAL_INIT_ADJ		(255)

struct mdss_mdp_kcal_pcc {
	uint32_t red;
	uint32_t green;
	uint32_t blue;
};

struct mdss_mdp_kcal_pa {
	uint32_t hue;
	uint32_t saturation;
	uint32_t value;
	uint32_t contrast;
};

struct kcal_lut_data {
	struct mdss_mdp_kcal_pcc pcc;
	struct mdss_mdp_kcal_pa pa;

	uint32_t enabled:1;
	uint32_t min;
};

/**
 * mdss_mdp_get_ctl() - get MDP control data of a specified display.
 * @index: index of a display in mdss data.
 */
static inline struct mdss_mdp_ctl *mdss_mdp_get_ctl(int index)
{
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	struct mdss_mdp_ctl *ctl;
	int i;

	/* Even if this is unlikely to happen, be safe for the future */
	if (IS_ERR_OR_NULL(mdata))
		return NULL;

	for (i = 0; i < mdata->nctl; ++i) {
		ctl = mdata->ctl_off + i;
		/* We need to setup a specified display only */
		if (ctl && ctl->mfd && ctl->mfd->panel_info->fb_num == index)
			return ctl;
	}

	return NULL;
}

static void mdss_mdp_kcal_read_pcc(struct kcal_lut_data *lut_data)
{
	struct mdss_mdp_ctl *ctl = mdss_mdp_get_ctl(0);
	struct mdp_pcc_data_v1_7 *pcc_data;
	struct mdp_pcc_cfg_data pcc_config = {
		.version = mdp_pcc_v1_7,
		.block   = MDP_LOGICAL_BLOCK_DISP_0,
		.ops     = MDP_PP_OPS_READ,
	};
	u32 copyback;

	pcc_data = kzalloc(sizeof(*pcc_data), GFP_KERNEL);
	if (IS_ERR_OR_NULL(pcc_data)) {
		pr_err("Unable to allocate memory for mdp payload\n");
		return;
	}

	/* Try to get current PCC configuration */
	pcc_config.cfg_payload = pcc_data;
	if (likely(ctl))
		mdss_mdp_pcc_config(ctl->mfd, &pcc_config, &copyback);

	/* Return early if something went wrong */
	if (unlikely(!pcc_data->r.r || !pcc_data->g.g || !pcc_data->b.b)) {
		kfree(pcc_data);
		return;
	}

	/*
	 * We need to get lower 2 bytes only as upper 2 are used
	 * by inversion mode implementation via PCC. Also note that
	 * the data stored in pcc_config is within SHRT limits, hence
	 * we need to divide it by 2^7 to convert to CHAR type bounds.
	 */
	lut_data->pcc.red   = (pcc_data->r.r & 0xFFFF) / PCC_ADJ;
	lut_data->pcc.green = (pcc_data->g.g & 0xFFFF) / PCC_ADJ;
	lut_data->pcc.blue  = (pcc_data->b.b & 0xFFFF) / PCC_ADJ;

	kfree(pcc_data);
}

static void mdss_mdp_kcal_update_pcc(struct kcal_lut_data *lut_data)
{
	struct mdss_mdp_ctl *ctl = mdss_mdp_get_ctl(0);
	struct mdp_pcc_data_v1_7 *cfg_payload;
	struct mdp_pcc_cfg_data pcc_config = {
		.version = mdp_pcc_v1_7,
		.block   = MDP_LOGICAL_BLOCK_DISP_0,
		.ops     = MDP_PP_OPS_WRITE  | (lut_data->enabled ?
			   MDP_PP_OPS_ENABLE : MDP_PP_OPS_DISABLE),
		.r = { .r = max(lut_data->pcc.red,   lut_data->min) * PCC_ADJ },
		.g = { .g = max(lut_data->pcc.green, lut_data->min) * PCC_ADJ },
		.b = { .b = max(lut_data->pcc.blue,  lut_data->min) * PCC_ADJ },
	};
	u32 copyback;

	cfg_payload = kzalloc(sizeof(*cfg_payload), GFP_KERNEL);
	if (IS_ERR_OR_NULL(cfg_payload)) {
		pr_err("Unable to allocate memory for mdp payload\n");
		return;
	}

	cfg_payload->r.r = pcc_config.r.r;
	cfg_payload->g.g = pcc_config.g.g;
	cfg_payload->b.b = pcc_config.b.b;
	pcc_config.cfg_payload = cfg_payload;

	/* Push PCC configuration to MDSS panel */
	if (likely(ctl))
		mdss_mdp_pcc_config(ctl->mfd, &pcc_config, &copyback);

	kfree(cfg_payload);
}

static void mdss_mdp_kcal_update_pa(struct kcal_lut_data *lut_data)
{
	struct mdss_mdp_ctl *ctl = mdss_mdp_get_ctl(0);
	struct mdp_pa_data_v1_7 *cfg_payload;
	struct mdp_pa_v2_cfg_data pa_v2_config = {
		.version = mdp_pa_v1_7,
		.block   = MDP_LOGICAL_BLOCK_DISP_0,
		.pa_v2_data = {
			.flags = MDP_PP_OPS_WRITE    | (lut_data->enabled ?
				 MDP_PP_OPS_ENABLE   : MDP_PP_OPS_DISABLE) |
				 MDP_PP_PA_HUE_MASK  | MDP_PP_PA_HUE_ENABLE |
				 MDP_PP_PA_SAT_MASK  | MDP_PP_PA_SAT_ENABLE |
				 MDP_PP_PA_VAL_MASK  | MDP_PP_PA_VAL_ENABLE |
				 MDP_PP_PA_CONT_MASK | MDP_PP_PA_CONT_ENABLE,
			.global_hue_adj  = lut_data->pa.hue,
			.global_sat_adj  = lut_data->pa.saturation,
			.global_val_adj  = lut_data->pa.value,
			.global_cont_adj = lut_data->pa.contrast,
		},
	};
	u32 copyback;

	pa_v2_config.flags = pa_v2_config.pa_v2_data.flags;

	cfg_payload = kzalloc(sizeof(*cfg_payload), GFP_KERNEL);
	if (IS_ERR_OR_NULL(cfg_payload)) {
		pr_err("Unable to allocate memory for mdp payload\n");
		return;
	}

	cfg_payload->mode = pa_v2_config.flags;
	cfg_payload->global_hue_adj  = pa_v2_config.pa_v2_data.global_hue_adj;
	cfg_payload->global_sat_adj  = pa_v2_config.pa_v2_data.global_sat_adj;
	cfg_payload->global_val_adj  = pa_v2_config.pa_v2_data.global_val_adj;
	cfg_payload->global_cont_adj = pa_v2_config.pa_v2_data.global_cont_adj;
	pa_v2_config.cfg_payload = cfg_payload;

	/* Push PA configuration to MDSS panel */
	if (likely(ctl))
		mdss_mdp_pa_v2_config(ctl->mfd, &pa_v2_config, &copyback);

	kfree(cfg_payload);
}

#define create_one_rw_node(node)					\
static DEVICE_ATTR(node, 0644, show_##node, store_##node)

#define define_one_kcal_node(node, object, min, max, update_pa)		\
static ssize_t show_##node(struct device *dev,				\
			   struct device_attribute *attr,		\
			   char *buf)					\
{									\
	struct kcal_lut_data *lut_data;					\
									\
	lut_data = dev_get_drvdata(dev);				\
	if (IS_ERR_OR_NULL(lut_data))					\
		return scnprintf(buf, 15, "<unsupported>\n");		\
									\
	return scnprintf(buf, 6, "%u\n", lut_data->object);		\
}									\
									\
static ssize_t store_##node(struct device *dev,				\
			    struct device_attribute *attr,		\
			    const char *buf, size_t count)		\
{									\
	struct kcal_lut_data *lut_data;					\
	uint32_t val;							\
	int ret;							\
									\
	lut_data = dev_get_drvdata(dev);				\
	if (IS_ERR_OR_NULL(lut_data))					\
		return -ENODEV;						\
									\
	ret = kstrtouint(buf, 10, &val);				\
	if (ret || val < min || val > max)				\
		return -EINVAL;						\
									\
	lut_data->object = val;						\
									\
	mdss_mdp_kcal_update_pcc(lut_data);				\
	if (update_pa)							\
		mdss_mdp_kcal_update_pa(lut_data);			\
									\
	return count;							\
}									\
									\
create_one_rw_node(node)

static ssize_t show_kcal(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	struct kcal_lut_data *lut_data;
	struct mdss_mdp_kcal_pcc *pcc;

	lut_data = dev_get_drvdata(dev);
	if (IS_ERR_OR_NULL(lut_data))
		return scnprintf(buf, 15, "<unsupported>\n");

	/* Always provide real values read from PCC registers */
	mdss_mdp_kcal_read_pcc(lut_data);
	pcc = &lut_data->pcc;

	return scnprintf(buf, 13, "%u %u %u\n",
			 pcc->red, pcc->green, pcc->blue);
}

static ssize_t store_kcal(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct kcal_lut_data *lut_data;
	uint32_t kcal_r, kcal_g, kcal_b;
	int ret;

	lut_data = dev_get_drvdata(dev);
	if (IS_ERR_OR_NULL(lut_data))
		return -ENODEV;

	ret = sscanf(buf, "%u %u %u", &kcal_r, &kcal_g, &kcal_b);
	if (ret != 3 ||
	    kcal_r < 1 || kcal_r > 256 ||
	    kcal_g < 1 || kcal_g > 256 ||
	    kcal_b < 1 || kcal_b > 256)
		return -EINVAL;

	lut_data->pcc.red = kcal_r;
	lut_data->pcc.green = kcal_g;
	lut_data->pcc.blue = kcal_b;

	mdss_mdp_kcal_update_pcc(lut_data);

	return count;
}

create_one_rw_node(kcal);
define_one_kcal_node(kcal_enable, enabled, 0, 1, true);
define_one_kcal_node(kcal_min, min, 1, 256, false);
define_one_kcal_node(kcal_hue, pa.hue, 0, 1536, true);
define_one_kcal_node(kcal_sat, pa.saturation, 128, 383, true);
define_one_kcal_node(kcal_val, pa.value, 128, 383, true);
define_one_kcal_node(kcal_cont, pa.contrast, 128, 383, true);

static int kcal_ctrl_probe(struct platform_device *pdev)
{
	struct kcal_lut_data *lut_data;
	int ret;

	lut_data = devm_kzalloc(&pdev->dev, sizeof(*lut_data), GFP_KERNEL);
	if (IS_ERR_OR_NULL(lut_data)) {
		pr_err("Unable to allocate memory for LUT data\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, lut_data);

	lut_data->enabled       = MDSS_MDP_KCAL_ENABLED;
	lut_data->min           = MDSS_MDP_KCAL_MIN_VALUE;
	lut_data->pcc.red       = MDSS_MDP_KCAL_INIT_RED;
	lut_data->pcc.green     = MDSS_MDP_KCAL_INIT_GREEN;
	lut_data->pcc.blue      = MDSS_MDP_KCAL_INIT_BLUE;
	lut_data->pa.hue        = MDSS_MDP_KCAL_INIT_HUE;
	lut_data->pa.saturation = MDSS_MDP_KCAL_INIT_ADJ;
	lut_data->pa.value      = MDSS_MDP_KCAL_INIT_ADJ;
	lut_data->pa.contrast   = MDSS_MDP_KCAL_INIT_ADJ;

	ret  = device_create_file(&pdev->dev, &dev_attr_kcal);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_enable);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_min);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_hue);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_sat);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_val);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_cont);
	if (ret) {
		pr_err("Unable to create sysfs nodes\n");
		goto fail;
	}

	mdss_mdp_kcal_update_pcc(lut_data);
	mdss_mdp_kcal_update_pa(lut_data);

	return 0;

fail:
	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, lut_data);

	return ret;
}

static int kcal_ctrl_remove(struct platform_device *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_kcal_cont);
	device_remove_file(&pdev->dev, &dev_attr_kcal_val);
	device_remove_file(&pdev->dev, &dev_attr_kcal_sat);
	device_remove_file(&pdev->dev, &dev_attr_kcal_hue);
	device_remove_file(&pdev->dev, &dev_attr_kcal_min);
	device_remove_file(&pdev->dev, &dev_attr_kcal_enable);
	device_remove_file(&pdev->dev, &dev_attr_kcal);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver kcal_ctrl_driver = {
	.probe = kcal_ctrl_probe,
	.remove = kcal_ctrl_remove,
	.driver = {
		.name = KCAL_CTRL,
		.owner = THIS_MODULE,
	},
};

static struct platform_device kcal_ctrl_device = {
	.name = KCAL_CTRL,
};

static int __init kcal_ctrl_init(void)
{
	int ret;

	ret = platform_driver_register(&kcal_ctrl_driver);
	if (ret) {
		pr_err("Unable to register platform driver\n");
		return ret;
	}

	ret = platform_device_register(&kcal_ctrl_device);
	if (ret) {
		pr_err("Unable to register platform device\n");
		platform_driver_unregister(&kcal_ctrl_driver);
		return ret;
	}

	return 0;
}
late_initcall(kcal_ctrl_init);
