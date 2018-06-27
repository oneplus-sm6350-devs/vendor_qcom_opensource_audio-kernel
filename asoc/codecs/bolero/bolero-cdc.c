/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include <linux/kernel.h>

#include "bolero-cdc.h"
#include "internal.h"

#define BOLERO_VERSION_1_0 0x0001
#define BOLERO_VERSION_ENTRY_SIZE 32

static struct snd_soc_codec_driver bolero;

/* MCLK_MUX table for all macros */
static u16 bolero_mclk_mux_tbl[MAX_MACRO][MCLK_MUX_MAX] = {
	{TX_MACRO, VA_MACRO},
	{TX_MACRO, RX_MACRO},
	{TX_MACRO, WSA_MACRO},
	{TX_MACRO, VA_MACRO},
};

static void bolero_ahb_write_device(char __iomem *io_base,
				    u16 reg, u8 value)
{
	u32 temp = (u32)(value) & 0x000000FF;

	iowrite32(temp, io_base + reg);
}

static void bolero_ahb_read_device(char __iomem *io_base,
				   u16 reg, u8 *value)
{
	u32 temp;

	temp = ioread32(io_base + reg);
	*value = (u8)temp;
}

static int __bolero_reg_read(struct bolero_priv *priv,
			     u16 macro_id, u16 reg, u8 *val)
{
	int ret = -EINVAL;
	u16 current_mclk_mux_macro;

	mutex_lock(&priv->clk_lock);
	current_mclk_mux_macro =
		priv->current_mclk_mux_macro[macro_id];
	if (!priv->macro_params[current_mclk_mux_macro].mclk_fn) {
		dev_dbg_ratelimited(priv->dev,
			"%s: mclk_fn not init for macro-id:%d, current_mclk_mux_macro:%d\n",
			__func__, macro_id, current_mclk_mux_macro);
		goto err;
	}
	ret = priv->macro_params[current_mclk_mux_macro].mclk_fn(
			priv->macro_params[current_mclk_mux_macro].dev, true);
	if (ret) {
		dev_dbg_ratelimited(priv->dev,
			"%s: clock enable failed for macro-id:%d, current_mclk_mux_macro:%d\n",
			__func__, macro_id, current_mclk_mux_macro);
		goto err;
	}
	bolero_ahb_read_device(
		priv->macro_params[macro_id].io_base, reg, val);
	priv->macro_params[current_mclk_mux_macro].mclk_fn(
			priv->macro_params[current_mclk_mux_macro].dev, false);
err:
	mutex_unlock(&priv->clk_lock);
	return ret;
}

static int __bolero_reg_write(struct bolero_priv *priv,
			      u16 macro_id, u16 reg, u8 val)
{
	int ret = -EINVAL;
	u16 current_mclk_mux_macro;

	mutex_lock(&priv->clk_lock);
	current_mclk_mux_macro =
		priv->current_mclk_mux_macro[macro_id];
	if (!priv->macro_params[current_mclk_mux_macro].mclk_fn) {
		dev_dbg_ratelimited(priv->dev,
			"%s: mclk_fn not init for macro-id:%d, current_mclk_mux_macro:%d\n",
			__func__, macro_id, current_mclk_mux_macro);
		goto err;
	}
	ret = priv->macro_params[current_mclk_mux_macro].mclk_fn(
			priv->macro_params[current_mclk_mux_macro].dev, true);
	if (ret) {
		dev_dbg_ratelimited(priv->dev,
			"%s: clock enable failed for macro-id:%d, current_mclk_mux_macro:%d\n",
			__func__, macro_id, current_mclk_mux_macro);
		goto err;
	}
	bolero_ahb_write_device(
		priv->macro_params[macro_id].io_base, reg, val);
	priv->macro_params[current_mclk_mux_macro].mclk_fn(
			priv->macro_params[current_mclk_mux_macro].dev, false);
err:
	mutex_unlock(&priv->clk_lock);
	return ret;
}

static bool bolero_is_valid_macro_dev(struct device *dev)
{
	if (of_device_is_compatible(dev->parent->of_node, "qcom,bolero-codec"))
		return true;

	return false;
}

static bool bolero_is_valid_codec_dev(struct device *dev)
{
	if (of_device_is_compatible(dev->of_node, "qcom,bolero-codec"))
		return true;

	return false;
}

/**
 * bolero_get_device_ptr - Get child or macro device ptr
 *
 * @dev: bolero device ptr.
 * @macro_id: ID of macro calling this API.
 *
 * Returns dev ptr on success or NULL on error.
 */
struct device *bolero_get_device_ptr(struct device *dev, u16 macro_id)
{
	struct bolero_priv *priv;

	if (!dev) {
		pr_err("%s: dev is null\n", __func__);
		return NULL;
	}

	if (!bolero_is_valid_codec_dev(dev)) {
		pr_err("%s: invalid codec\n", __func__);
		return NULL;
	}
	priv = dev_get_drvdata(dev);
	if (!priv || (macro_id >= MAX_MACRO)) {
		dev_err(dev, "%s: priv is null or invalid macro\n", __func__);
		return NULL;
	}

	return priv->macro_params[macro_id].dev;
}
EXPORT_SYMBOL(bolero_get_device_ptr);

static int bolero_copy_dais_from_macro(struct bolero_priv *priv)
{
	struct snd_soc_dai_driver *dai_ptr;
	u16 macro_idx;

	/* memcpy into bolero_dais all macro dais */
	if (!priv->bolero_dais)
		priv->bolero_dais = devm_kzalloc(priv->dev,
						priv->num_dais *
						sizeof(
						struct snd_soc_dai_driver),
						GFP_KERNEL);
	if (!priv->bolero_dais)
		return -ENOMEM;

	dai_ptr = priv->bolero_dais;

	for (macro_idx = START_MACRO; macro_idx < MAX_MACRO; macro_idx++) {
		if (priv->macro_params[macro_idx].dai_ptr) {
			memcpy(dai_ptr,
			       priv->macro_params[macro_idx].dai_ptr,
			       priv->macro_params[macro_idx].num_dais *
			       sizeof(struct snd_soc_dai_driver));
			dai_ptr += priv->macro_params[macro_idx].num_dais;
		}
	}
	return 0;
}

/**
 * bolero_register_macro - Registers macro to bolero
 *
 * @dev: macro device ptr.
 * @macro_id: ID of macro calling this API.
 * @ops: macro params to register.
 *
 * Returns 0 on success or -EINVAL on error.
 */
int bolero_register_macro(struct device *dev, u16 macro_id,
			  struct macro_ops *ops)
{
	struct bolero_priv *priv;
	int ret = -EINVAL;

	if (!dev || !ops) {
		pr_err("%s: dev or ops is null\n", __func__);
		return -EINVAL;
	}
	if (!bolero_is_valid_macro_dev(dev)) {
		dev_err(dev, "%s: child device for macro:%d not added yet\n",
			__func__, macro_id);
		return -EINVAL;
	}
	priv = dev_get_drvdata(dev->parent);
	if (!priv || (macro_id >= MAX_MACRO)) {
		dev_err(dev, "%s: priv is null or invalid macro\n", __func__);
		return -EINVAL;
	}

	priv->macro_params[macro_id].init = ops->init;
	priv->macro_params[macro_id].exit = ops->exit;
	priv->macro_params[macro_id].io_base = ops->io_base;
	priv->macro_params[macro_id].num_dais = ops->num_dais;
	priv->macro_params[macro_id].dai_ptr = ops->dai_ptr;
	priv->macro_params[macro_id].mclk_fn = ops->mclk_fn;
	priv->macro_params[macro_id].dev = dev;
	priv->current_mclk_mux_macro[macro_id] =
				bolero_mclk_mux_tbl[macro_id][MCLK_MUX0];
	priv->num_dais += ops->num_dais;
	priv->num_macros_registered++;
	priv->macros_supported[macro_id] = true;

	if (priv->num_macros_registered == priv->child_num) {
		ret = bolero_copy_dais_from_macro(priv);
		if (ret < 0) {
			dev_err(dev, "%s: copy_dais failed\n", __func__);
			return ret;
		}
		if (priv->macros_supported[TX_MACRO] == false) {
			bolero_mclk_mux_tbl[WSA_MACRO][MCLK_MUX0] = WSA_MACRO;
			priv->current_mclk_mux_macro[WSA_MACRO] = WSA_MACRO;
			bolero_mclk_mux_tbl[VA_MACRO][MCLK_MUX0] = VA_MACRO;
			priv->current_mclk_mux_macro[VA_MACRO] = VA_MACRO;
		}
		ret = snd_soc_register_codec(dev->parent, &bolero,
				priv->bolero_dais, priv->num_dais);
		if (ret < 0) {
			dev_err(dev, "%s: register codec failed\n", __func__);
			return ret;
		}
	}
	return 0;
}
EXPORT_SYMBOL(bolero_register_macro);

/**
 * bolero_unregister_macro - De-Register macro from bolero
 *
 * @dev: macro device ptr.
 * @macro_id: ID of macro calling this API.
 *
 */
void bolero_unregister_macro(struct device *dev, u16 macro_id)
{
	struct bolero_priv *priv;

	if (!dev) {
		pr_err("%s: dev is null\n", __func__);
		return;
	}
	if (!bolero_is_valid_macro_dev(dev)) {
		dev_err(dev, "%s: macro:%d not in valid registered macro-list\n",
			__func__, macro_id);
		return;
	}
	priv = dev_get_drvdata(dev->parent);
	if (!priv || (macro_id >= MAX_MACRO)) {
		dev_err(dev, "%s: priv is null or invalid macro\n", __func__);
		return;
	}

	priv->macro_params[macro_id].init = NULL;
	priv->macro_params[macro_id].num_dais = 0;
	priv->macro_params[macro_id].dai_ptr = NULL;
	priv->macro_params[macro_id].mclk_fn = NULL;
	priv->macro_params[macro_id].dev = NULL;
	priv->num_dais -= priv->macro_params[macro_id].num_dais;
	priv->num_macros_registered--;

	/* UNREGISTER CODEC HERE */
	if (priv->child_num - 1 == priv->num_macros_registered)
		snd_soc_unregister_codec(dev->parent);
}
EXPORT_SYMBOL(bolero_unregister_macro);

/**
 * bolero_request_clock - request for clock enable/disable
 *
 * @dev: macro device ptr.
 * @macro_id: ID of macro calling this API.
 * @mclk_mux_id: MCLK_MUX ID.
 * @enable: enable or disable clock flag
 *
 * Returns 0 on success or -EINVAL on error.
 */
int bolero_request_clock(struct device *dev, u16 macro_id,
			 enum mclk_mux mclk_mux_id,
			 bool enable)
{
	struct bolero_priv *priv;
	u16 mclk_mux0_macro, mclk_mux1_macro;
	int ret = 0;

	if (!dev) {
		pr_err("%s: dev is null\n", __func__);
		return -EINVAL;
	}
	if (!bolero_is_valid_macro_dev(dev)) {
		dev_err(dev, "%s: macro:%d not in valid registered macro-list\n",
			__func__, macro_id);
		return -EINVAL;
	}
	priv = dev_get_drvdata(dev->parent);
	if (!priv || (macro_id >= MAX_MACRO)) {
		dev_err(dev, "%s: priv is null or invalid macro\n", __func__);
		return -EINVAL;
	}
	mclk_mux0_macro =  bolero_mclk_mux_tbl[macro_id][MCLK_MUX0];
	mutex_lock(&priv->clk_lock);
	switch (mclk_mux_id) {
	case MCLK_MUX0:
		ret = priv->macro_params[mclk_mux0_macro].mclk_fn(
			priv->macro_params[mclk_mux0_macro].dev, enable);
		if (ret < 0) {
			dev_err(dev,
				"%s: MCLK_MUX0 %s failed for macro:%d, mclk_mux0_macro:%d\n",
				__func__,
				enable ? "enable" : "disable",
				macro_id, mclk_mux0_macro);
			goto err;
		}
		break;
	case MCLK_MUX1:
		mclk_mux1_macro =  bolero_mclk_mux_tbl[macro_id][MCLK_MUX1];
		ret = priv->macro_params[mclk_mux0_macro].mclk_fn(
			priv->macro_params[mclk_mux0_macro].dev,
			true);
		if (ret < 0) {
			dev_err(dev,
				"%s: MCLK_MUX0 en failed for macro:%d mclk_mux0_macro:%d\n",
				__func__, macro_id, mclk_mux0_macro);
			goto err;
		}
		ret = priv->macro_params[mclk_mux1_macro].mclk_fn(
			priv->macro_params[mclk_mux1_macro].dev, enable);
		if (ret < 0) {
			dev_err(dev,
				"%s: MCLK_MUX1 %s failed for macro:%d, mclk_mux1_macro:%d\n",
				__func__,
				enable ? "enable" : "disable",
				macro_id, mclk_mux1_macro);
			priv->macro_params[mclk_mux0_macro].mclk_fn(
				priv->macro_params[mclk_mux0_macro].dev,
				false);
			goto err;
		}
		priv->macro_params[mclk_mux0_macro].mclk_fn(
			priv->macro_params[mclk_mux0_macro].dev,
			false);
		break;
	case MCLK_MUX_MAX:
	default:
		dev_err(dev, "%s: invalid mclk_mux_id: %d\n",
			__func__, mclk_mux_id);
		ret = -EINVAL;
		goto err;
	}
	if (enable)
		priv->current_mclk_mux_macro[macro_id] =
				bolero_mclk_mux_tbl[macro_id][mclk_mux_id];
	else
		priv->current_mclk_mux_macro[macro_id] =
				bolero_mclk_mux_tbl[macro_id][MCLK_MUX0];
err:
	mutex_unlock(&priv->clk_lock);
	return ret;
}
EXPORT_SYMBOL(bolero_request_clock);

static ssize_t bolero_version_read(struct snd_info_entry *entry,
				   void *file_private_data,
				   struct file *file,
				   char __user *buf, size_t count,
				   loff_t pos)
{
	struct bolero_priv *priv;
	char buffer[BOLERO_VERSION_ENTRY_SIZE];
	int len = 0;

	priv = (struct bolero_priv *) entry->private_data;
	if (!priv) {
		pr_err("%s: bolero priv is null\n", __func__);
		return -EINVAL;
	}

	switch (priv->version) {
	case BOLERO_VERSION_1_0:
		len = snprintf(buffer, sizeof(buffer), "BOLERO_1_0\n");
		break;
	default:
		len = snprintf(buffer, sizeof(buffer), "VER_UNDEFINED\n");
	}

	return simple_read_from_buffer(buf, count, &pos, buffer, len);
}

static struct snd_info_entry_ops bolero_info_ops = {
	.read = bolero_version_read,
};

/*
 * bolero_info_create_codec_entry - creates bolero module
 * @codec_root: The parent directory
 * @codec: Codec instance
 *
 * Creates bolero module and version entry under the given
 * parent directory.
 *
 * Return: 0 on success or negative error code on failure.
 */
int bolero_info_create_codec_entry(struct snd_info_entry *codec_root,
				   struct snd_soc_codec *codec)
{
	struct snd_info_entry *version_entry;
	struct bolero_priv *priv;
	struct snd_soc_card *card;

	if (!codec_root || !codec)
		return -EINVAL;

	priv = snd_soc_codec_get_drvdata(codec);
	if (priv->entry) {
		dev_dbg(priv->dev,
			"%s:bolero module already created\n", __func__);
		return 0;
	}
	card = codec->component.card;
	priv->entry = snd_info_create_subdir(codec_root->module,
					     "bolero", codec_root);
	if (!priv->entry) {
		dev_dbg(codec->dev, "%s: failed to create bolero entry\n",
			__func__);
		return -ENOMEM;
	}
	version_entry = snd_info_create_card_entry(card->snd_card,
						   "version",
						   priv->entry);
	if (!version_entry) {
		dev_err(codec->dev, "%s: failed to create bolero version entry\n",
			__func__);
		return -ENOMEM;
	}

	version_entry->private_data = priv;
	version_entry->size = BOLERO_VERSION_ENTRY_SIZE;
	version_entry->content = SNDRV_INFO_CONTENT_DATA;
	version_entry->c.ops = &bolero_info_ops;

	if (snd_info_register(version_entry) < 0) {
		snd_info_free_entry(version_entry);
		return -ENOMEM;
	}
	priv->version_entry = version_entry;

	return 0;
}
EXPORT_SYMBOL(bolero_info_create_codec_entry);

static int bolero_soc_codec_probe(struct snd_soc_codec *codec)
{
	struct bolero_priv *priv = dev_get_drvdata(codec->dev);
	int macro_idx, ret = 0;

	/* call init for supported macros */
	for (macro_idx = START_MACRO; macro_idx < MAX_MACRO; macro_idx++) {
		if (priv->macro_params[macro_idx].init) {
			ret = priv->macro_params[macro_idx].init(codec);
			if (ret < 0) {
				dev_err(codec->dev,
					"%s: init for macro %d failed\n",
					__func__, macro_idx);
				goto err;
			}
		}
	}
	priv->codec = codec;
	priv->version = BOLERO_VERSION_1_0;
	dev_dbg(codec->dev, "%s: bolero soc codec probe success\n", __func__);
err:
	return ret;
}

static int bolero_soc_codec_remove(struct snd_soc_codec *codec)
{
	struct bolero_priv *priv = dev_get_drvdata(codec->dev);
	int macro_idx;

	/* call exit for supported macros */
	for (macro_idx = START_MACRO; macro_idx < MAX_MACRO; macro_idx++)
		if (priv->macro_params[macro_idx].exit)
			priv->macro_params[macro_idx].exit(codec);

	return 0;
}

static struct regmap *bolero_get_regmap(struct device *dev)
{
	struct bolero_priv *priv = dev_get_drvdata(dev);

	return priv->regmap;
}

static struct snd_soc_codec_driver bolero = {
	.probe = bolero_soc_codec_probe,
	.remove = bolero_soc_codec_remove,
	.get_regmap = bolero_get_regmap,
};

static void bolero_add_child_devices(struct work_struct *work)
{
	struct bolero_priv *priv;
	int rc;

	priv = container_of(work, struct bolero_priv,
			    bolero_add_child_devices_work);
	if (!priv) {
		pr_err("%s: Memory for bolero priv does not exist\n",
			__func__);
		return;
	}
	if (!priv->dev || !priv->dev->of_node) {
		dev_err(priv->dev, "%s: DT node for bolero does not exist\n",
			__func__);
		return;
	}
	rc = of_platform_populate(priv->dev->of_node, NULL, NULL, priv->dev);
	if (rc)
		dev_err(priv->dev, "%s: failed to add child nodes, rc=%d\n",
			__func__, rc);
	else
		dev_dbg(priv->dev, "%s: added child node\n", __func__);
}

static int bolero_probe(struct platform_device *pdev)
{
	struct bolero_priv *priv;
	u32 num_macros = 0;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct bolero_priv),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	ret = of_property_read_u32(pdev->dev.of_node, "qcom,num-macros",
				   &num_macros);
	if (ret) {
		dev_err(&pdev->dev,
			"%s:num-macros property not found\n",
			__func__);
		return ret;
	}
	priv->child_num = num_macros;
	if (priv->child_num > MAX_MACRO) {
		dev_err(&pdev->dev,
			"%s:child_num(%d) > MAX_MACRO(%d) than supported\n",
			__func__, priv->child_num, MAX_MACRO);
		return -EINVAL;
	}
	priv->va_without_decimation = of_property_read_bool(pdev->dev.of_node,
						"qcom,va-without-decimation");
	if (priv->va_without_decimation)
		bolero_reg_access[VA_MACRO] = bolero_va_top_reg_access;

	priv->dev = &pdev->dev;
	priv->regmap = bolero_regmap_init(priv->dev,
					  &bolero_regmap_config);
	if (IS_ERR_OR_NULL((void *)(priv->regmap))) {
		dev_err(&pdev->dev, "%s:regmap init failed\n", __func__);
		return -EINVAL;
	}
	priv->read_dev = __bolero_reg_read;
	priv->write_dev = __bolero_reg_write;

	dev_set_drvdata(&pdev->dev, priv);
	mutex_init(&priv->io_lock);
	mutex_init(&priv->clk_lock);
	INIT_WORK(&priv->bolero_add_child_devices_work,
		  bolero_add_child_devices);
	schedule_work(&priv->bolero_add_child_devices_work);

	return 0;
}

static int bolero_remove(struct platform_device *pdev)
{
	struct bolero_priv *priv = dev_get_drvdata(&pdev->dev);

	if (!priv)
		return -EINVAL;

	of_platform_depopulate(&pdev->dev);
	mutex_destroy(&priv->io_lock);
	mutex_destroy(&priv->clk_lock);
	return 0;
}

static const struct of_device_id bolero_dt_match[] = {
	{.compatible = "qcom,bolero-codec"},
	{}
};
MODULE_DEVICE_TABLE(of, bolero_dt_match);

static struct platform_driver bolero_drv = {
	.driver = {
		.name = "bolero-codec",
		.owner = THIS_MODULE,
		.of_match_table = bolero_dt_match,
	},
	.probe = bolero_probe,
	.remove = bolero_remove,
};

module_platform_driver(bolero_drv);

MODULE_DESCRIPTION("Bolero driver");
MODULE_LICENSE("GPL v2");