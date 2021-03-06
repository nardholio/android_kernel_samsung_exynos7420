/*
 * es705-platform.c  --  Audience eS705 platform dependent functions
 *
 * Copyright 2011 Audience, Inc.
 *
 * Author: Genisim Tsilker <gtsilker@audience.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define DEBUG

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/esxxx.h>

#include "es705.h"
#include "es705-platform.h"

#if defined(SAMSUNG_ES705_FEATURE)
#include <linux/input.h>
#endif
#ifdef CONFIG_ARCH_EXYNOS /* EXYNOS CLK ENABLE */
#include <linux/io.h>
#include <mach/exynos-audio.h>
#endif

static int stimulate_only_once;
static int reset_gpio;

int es705_get_stimulate_status(void)
{
	return stimulate_only_once;
}

int es705_enable_ext_clk(void *priv_data, int enable)
{
	struct es705_priv *es705 = (struct es705_priv *)priv_data;
	int rc = 0;

	dev_info(es705->dev, "%s: clk enable=%d\n", __func__, enable);

#ifdef CONFIG_ARCH_EXYNOS
	if (enable) {
		if(!IS_ERR(es705->mclk)) {
			rc = clk_prepare_enable(es705->mclk);
			if (rc) {
				dev_err(es705->dev, "%s: Failed to prepare or enable clock, err=%d\n", __func__, rc);
				goto err;
			}
		} else
			exynos5_audio_set_mclk(true, 0);
	} else {
		if(!IS_ERR(es705->mclk))
			clk_disable_unprepare(es705->mclk);
		else
			exynos5_audio_set_mclk(false, 0);
	}
	return 0;
err:
#else
	es705->mclk = clk_get(es705->dev, "osr_clk");
	if (!es705->mclk) {
		dev_err(es705->dev, "%s: clk_get osr_clk FAIL\n", __func__);
		return -ENODEV;
	}

	if (enable) {
		rc = clk_prepare_enable(es705->mclk);
		if (rc) {
			dev_err(es705->dev,
				"Failed to prepare or enable clock, err=%d\n",
				rc);
			goto err;
		}
	} else {
		clk_disable_unprepare(es705->mclk);
		clk_put(es705->mclk);
	}

	return 0;

err:
	clk_put(es705->mclk);
#endif
	return rc;
}

void es705_clk_init(struct es705_priv *es705)
{
	es705->pdata->esxxx_clk_cb = es705_enable_ext_clk;
}

int es705_stimulate_start(struct device *dev)
{
	int rc = 0;
	struct device_node *node = dev->of_node;

	if (stimulate_only_once) {
		dev_err(dev,
			"%s(): register node %s. es705 stimulate start already started\n",
			__func__, dev->of_node->full_name);
		return 0;
	}

	dev_dbg(dev, "%s(): register node %s\n",
		__func__, dev->of_node->full_name);

	/* Populte GPIO reset from DTS */
	reset_gpio = of_get_named_gpio(node, "esxxx-reset-gpio", 0);
	if (reset_gpio < 0) {
		of_property_read_u32(node, "esxxx-reset-expander-gpio",
				     &reset_gpio);
	}
	if (reset_gpio < 0) {
		dev_err(dev, "%s(): get reset_gpio failed\n", __func__);
		goto populate_reset_err;
	}
	dev_dbg(dev, "%s(): reset gpio %d\n", __func__, reset_gpio);

	/* Generate es705 reset */
	rc = gpio_request(reset_gpio, "es705_reset");
	if (rc < 0) {
		dev_err(dev, "%s(): es705_reset request failed", __func__);
		goto reset_gpio_request_error;
	}
	rc = gpio_direction_output(reset_gpio, 0);
	if (rc < 0) {
		dev_err(dev, "%s(): es705_reset direction failed", __func__);
		goto reset_gpio_direction_error;
	}

#if defined(CONFIG_SND_SOC_ES_SLIM)
	/* Enable es705 clock */
	es705_enable_ext_clk(dev, 1);
	usleep_range(5000, 5100);

	es705_gpio_reset(&es705_priv);
#endif

	stimulate_only_once = 1;

	return rc;

reset_gpio_direction_error:
	gpio_free(reset_gpio);
reset_gpio_request_error:
populate_reset_err:
	return rc;
}

void es705_gpio_reset(struct es705_priv *es705)
{
	dev_dbg(es705->dev, "%s(): GPIO reset\n", __func__);

#ifdef CONFIG_ARCH_EXYNOS
	gpio_set_value(reset_gpio, 0);

	/* Wait 1 ms then pull Reset signal in High */
	usleep_range(1000, 1100);

	gpio_set_value(reset_gpio, 1);
#else
	gpio_direction_output(es705->pdata->reset_gpio, 0);

	/* Wait 1 ms then pull Reset signal in High */
	usleep_range(1000, 1100);

	gpio_direction_output(es705->pdata->reset_gpio, 1);
#endif

	/* Wait 10 ms then */
	usleep_range(10000, 11000);
	/* eSxxx is READY */
}

int es705_gpio_init(struct es705_priv *es705)
{
	int rc = 0;
#if defined(CONFIG_SND_SOC_ESXXX_SEAMLESS_VOICE_WAKEUP)
	if (es705->pdata->gpiob_gpio) {
		rc = request_threaded_irq(es705->pdata->irq_base, NULL,
					  es705_irq_event,
					  IRQF_ONESHOT | IRQF_TRIGGER_RISING,
					  "es705-irq-event", es705);
		if (rc) {
			dev_err(es705->dev,
				"%s(): event request_irq() failed\n", __func__);
			goto event_irq_request_error;
		}

		rc = irq_set_irq_wake(es705->pdata->irq_base, 1);
		if (rc < 0) {
			dev_err(es705->dev, "%s(): set event irq wake failed\n",
				__func__);
			disable_irq(es705->pdata->irq_base);
			free_irq(es705->pdata->irq_base, es705);
			goto event_irq_wake_error;
		}
	}

	return rc;

event_irq_wake_error:
event_irq_request_error:
#endif
	return rc;
}

#if !defined(CONFIG_SND_SOC_ESXXX_UART_WAKEUP)
void es705_gpio_wakeup(struct es705_priv *es705)
{
	dev_info(es705->dev, "%s()\n", __func__);
	gpio_set_value(es705->pdata->wakeup_gpio, 1);
	usleep_range(1000, 1005);
	gpio_set_value(es705->pdata->wakeup_gpio, 0);
}

void es705_gpio_wakeup_deassert(struct es705_priv *es705)
{
	dev_info(es705->dev, "%s()\n", __func__);
	/* Deassert wakeup signal Low to High */
	gpio_set_value(es705->pdata->wakeup_gpio, 1);
}
#endif

#if defined(SAMSUNG_ES705_FEATURE)
void es705_uart_pin_preset(struct es705_priv *es705)
{
	/* reserved */
}

void es705_uart_pin_postset(struct es705_priv *es705)
{
	/* reserved */
}
#endif

#if defined(CONFIG_SND_SOC_ESXXX_SEAMLESS_VOICE_WAKEUP)
void es705_vs_event(struct es705_priv *es705)
{
	char keyword_buf[100];
	unsigned int keyword_reg = 0;
	char *envp[2];

	memset(keyword_buf, 0, sizeof(keyword_buf));

	if (es705->voice_wakeup_enable == 1) {	/* Voice wakeup */
		keyword_reg = es705_priv.detected_vs_keyword;
		es705->keyword_type = keyword_reg;
		snprintf(keyword_buf, sizeof(keyword_buf),
			 "VOICE_WAKEUP_WORD_ID=%x", keyword_reg);
	} else if (es705->voice_wakeup_enable == 2) {	/* Voice wakeup LPSD */
		snprintf(keyword_buf, sizeof(keyword_buf),
			 "VOICE_WAKEUP_WORD_ID=LPSD");
	} else {
		dev_info(es705->dev, "%s(): Invalid value(%d)\n", __func__,
			 es705->voice_wakeup_enable);
		return;
	}

	envp[0] = keyword_buf;
	envp[1] = NULL;

	dev_info(es705->dev, "%s : raise the uevent, string = %s\n", __func__,
		 keyword_buf);
	kobject_uevent_env(&(es705->keyword->kobj), KOBJ_CHANGE, envp);
}
#endif
void es705_gpio_free(struct esxxx_platform_data *pdata)
{
	if (pdata->reset_gpio != -1)
		gpio_free(pdata->reset_gpio);
}

struct esxxx_platform_data *es705_populate_dt_pdata(struct device *dev)
{
	struct esxxx_platform_data *pdata;
	struct device_node *node = dev->of_node;
	int rc = 0;
	u32 val = 0;

	dev_info(dev, "%s(): parent node %s\n", __func__, node->full_name);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "%s(): platform data allocation failed\n",
			__func__);
		return NULL;
	}

	pdata->reset_gpio = of_get_named_gpio(node, "esxxx-reset-gpio", 0);
	if (pdata->reset_gpio < 0)
		of_property_read_u32(node, "esxxx-reset-expander-gpio",
				     &pdata->reset_gpio);
	if (pdata->reset_gpio < 0) {
		dev_err(dev, "%s(): get reset_gpio failed\n", __func__);
		pdata->reset_gpio = -1;
	}
	dev_info(dev, "%s(): reset gpio %d\n", __func__, pdata->reset_gpio);

#if !defined(CONFIG_SND_SOC_ESXXX_UART_WAKEUP)
	pdata->wakeup_gpio = of_get_named_gpio(node, "esxxx-wakeup-gpio", 0);
	if (pdata->wakeup_gpio < 0) {
		dev_err(dev, "%s(): get wakeup_gpio failed\n", __func__);
		pdata->wakeup_gpio = -1;
	}
	dev_info(dev, "%s(): wakeup gpio %d\n", __func__, pdata->wakeup_gpio);

	/* Generate es705 wakup gpio */
	rc = gpio_request(pdata->wakeup_gpio, "es705_wakeup");
	if (rc < 0) {
		dev_err(dev, "%s(): es705_reset request failed", __func__);
	}
	rc = gpio_direction_output(pdata->wakeup_gpio, 1);
	if (rc < 0) {
		dev_err(dev, "%s(): es705_reset direction failed", __func__);
	}

#endif

#if defined(CONFIG_SND_SOC_ESXXX_SEAMLESS_VOICE_WAKEUP)
	/* VS IRQ */
	pdata->gpiob_gpio = of_get_named_gpio(node, "esxxx-gpiob-gpio", 0);
	if (pdata->gpiob_gpio < 0) {
		dev_err(dev, "%s(): get gpiob_gpio failed\n", __func__);
		pdata->gpiob_gpio = -1;
	}
	dev_info(dev, "%s(): gpiob gpio %d\n", __func__, pdata->gpiob_gpio);
	pdata->irq_base = gpio_to_irq(pdata->gpiob_gpio);
#endif

	pdata->uart_tx_gpio = of_get_named_gpio(node, "esxxx-uart-tx", 0);
	if (pdata->uart_tx_gpio < 0) {
		dev_err(dev, "%s(): get uart_tx_gpio failed\n", __func__);
		pdata->uart_tx_gpio = -1;
	}
	dev_info(dev, "%s(): uart tx gpio %d\n", __func__, pdata->uart_tx_gpio);

	pdata->uart_rx_gpio = of_get_named_gpio(node, "esxxx-uart-rx", 0);
	if (pdata->uart_rx_gpio < 0) {
		dev_err(dev, "%s(): get uart_rx_gpio failed\n", __func__);
		pdata->uart_rx_gpio = -1;
	}
	dev_info(dev, "%s(): uart rx gpio %d\n", __func__, pdata->uart_rx_gpio);

	rc = of_property_read_string(node, "adnc,firmware_name",&pdata->fw_filename);
	if (rc < 0)
		dev_info(dev,"%s(): no firmware_name in device tree\n",__func__);
	else
		dev_info(dev, "%s: fw_filename=%s", __func__, pdata->fw_filename);
#if defined(CONFIG_SND_SOC_ESXXX_SEAMLESS_VOICE_WAKEUP)
	rc = of_property_read_string(node, "adnc,vs_firmware_name",&pdata->vs_filename);
	if (rc < 0)
		dev_info(dev,"%s(): no vs_firmware_name in device tree\n",__func__);
	else
		dev_info(dev, "%s: vs_filename=%s", __func__, pdata->vs_filename);
#endif

	if (of_find_property(node, "adnc,use_dhwpt", NULL)) {
		pdata->use_dhwpt = 1;
		dev_info(dev, "%s(): adnc,use_dhwpt exist in device tree\n",
					__func__);
	} else {
		pdata->use_dhwpt = 0;
		dev_info(dev, "%s(): adnc,use_dhwpt not exist in device tree\n",
					__func__);
	}

	if (of_property_read_u32(node, "adnc,dhwpt_mode", &val)) {
		dev_info(dev, "%s(): adnc,dhwpt_mode not exist in device tree\n",
					__func__);
		/* Default DHWPT is B to D */
		pdata->dhwpt_mode = 0x5C;
	} else {
		dev_info(dev, "%s(): adnc,dhwpt_mode = 0x%x\n",
					__func__, val);
		pdata->dhwpt_mode = val;
	}

	if (of_find_property(node, "adnc,use_dhwpt_in_sbl", NULL)) {
		pdata->use_dhwpt_in_sbl = 1;
		dev_info(dev, "%s(): adnc,use_dhwpt_in_sbl exist in device tree\n",
					__func__);
	} else {
		pdata->use_dhwpt_in_sbl = 0;
		dev_info(dev, "%s(): adnc,use_dhwpt_in_sbl not exist in device tree\n",
					__func__);
	}

	return pdata;
}
