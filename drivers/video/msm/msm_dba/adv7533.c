/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/switch.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include "msm_dba_internal.h"
#include <linux/mdss_io_util.h>

#define ADV7533_REG_CHIP_REVISION (0x00)
#define ADV7533_RESET_DELAY (100)

#define PINCTRL_STATE_ACTIVE    "pmx_adv7533_active"
#define PINCTRL_STATE_SUSPEND   "pmx_adv7533_suspend"

#define MDSS_MAX_PANEL_LEN      256
#define EDID_SEG_SIZE 0x100

/* 0x94 interrupts */
#define HPD_INT_ENABLE           BIT(7)
#define MONITOR_SENSE_INT_ENABLE BIT(6)
#define ACTIVE_VSYNC_EDGE        BIT(5)
#define AUDIO_FIFO_FULL          BIT(4)
#define EDID_READY_INT_ENABLE    BIT(2)
#define HDCP_AUTHENTICATED       BIT(1)
#define HDCP_RI_READY            BIT(0)

/* 0x95 interrupts */
#define HDCP_ERROR               BIT(7)
#define HDCP_BKSV_FLAG           BIT(6)
#define CEC_TX_READY             BIT(5)
#define CEC_TX_ARB_LOST          BIT(4)
#define CEC_TX_RETRY_TIMEOUT     BIT(3)
#define CEC_TX_RX_BUF3_READY     BIT(2)
#define CEC_TX_RX_BUF2_READY     BIT(1)
#define CEC_TX_RX_BUF1_READY     BIT(0)

#define HPD_INTERRUPTS           (HPD_INT_ENABLE | \
					MONITOR_SENSE_INT_ENABLE)
#define EDID_INTERRUPTS          EDID_READY_INT_ENABLE
#define HDCP_INTERRUPTS1         HDCP_AUTHENTICATED
#define HDCP_INTERRUPTS2         (HDCP_BKSV_FLAG | \
					HDCP_ERROR)
#define CEC_INTERRUPTS           (CEC_TX_READY | \
					CEC_TX_ARB_LOST | \
					CEC_TX_RETRY_TIMEOUT | \
					CEC_TX_RX_BUF3_READY | \
					CEC_TX_RX_BUF2_READY | \
					CEC_TX_RX_BUF1_READY)

#define ADV7533_WRITE(addr, r, v) \
	do { \
		ret = msm_dba_helper_i2c_write_byte(pdata->i2c_client, \
					addr, r, v); \
		if (ret) { \
			pr_err("%s: wr err: addr 0x%x, reg 0x%x, val 0x%x\n", \
			__func__, addr, r, v); \
			goto end; \
		} \
	} while (0)

#define ADV7533_READ(addr, r, v, b) \
	do { \
		ret = msm_dba_helper_i2c_read(pdata->i2c_client, \
					addr, r, v, b); \
		if (ret) { \
			pr_err("%s: rd err: addr 0x%x, reg 0x%x\n", \
			__func__, addr, r); \
			goto end; \
		} \
	} while (0)

#define ADV7533_WRITE_ARRAY(cfg) \
	do { \
		int i = 0; \
		while (cfg[i].i2c_addr != I2C_ADDR_MAX) { \
			adv7533_write_byte(cfg[i].i2c_addr, \
				cfg[i].reg, cfg[i].val); \
			i++; \
		} \
	} while (0)

#define CFG_HPD_INTERRUPTS       BIT(0)
#define CFG_EDID_INTERRUPTS      BIT(1)
#define CFG_HDCP_INTERRUPTS      BIT(2)
#define CFG_CEC_INTERRUPTS       BIT(3)

#define MAX_OPERAND_SIZE	14
#define CEC_MSG_SIZE            (MAX_OPERAND_SIZE + 2)

enum adv7533_i2c_addr {
	I2C_ADDR_MAIN = 0x39,
	I2C_ADDR_CEC_DSI = 0x3C,
	I2C_ADDR_MAX = 0xFF,
};

enum adv7533_audio {
	ADV7533_AUDIO_OFF,
	ADV7533_AUDIO_ON,
};

enum adv7533_cec_buf {
	ADV7533_CEC_BUF1,
	ADV7533_CEC_BUF2,
	ADV7533_CEC_BUF3,
	ADV7533_CEC_BUF_MAX,
};

struct adv7533_reg_cfg {
	u8 i2c_addr;
	u8 reg;
	u8 val;
};

struct adv7533_cec_msg {
	u8 buf[CEC_MSG_SIZE];
	u8 timestamp;
	bool pending;
};

struct adv7533 {
	u8 main_i2c_addr;
	u8 cec_dsi_i2c_addr;
	u8 video_mode;
	int irq;
	u32 irq_gpio;
	u32 irq_flags;
	u32 hpd_irq_gpio;
	u32 hpd_irq_flags;
	u32 switch_gpio;
	u32 switch_flags;
	struct pinctrl *ts_pinctrl;
	struct pinctrl_state *pinctrl_state_active;
	struct pinctrl_state *pinctrl_state_suspend;
	bool audio;
	bool disable_gpios;
	bool adv_output;
	struct switch_dev audio_sdev;
	struct dss_module_power power_data;
	bool hdcp_enabled;
	bool cec_enabled;
	bool is_power_on;
	void *edid_data;
	u8 edid_buf[EDID_SEG_SIZE];
	struct workqueue_struct *workq;
	struct delayed_work adv7533_intr_work_id;
	struct msm_dba_device_info dev_info;
	struct adv7533_cec_msg cec_msg[ADV7533_CEC_BUF_MAX];
	struct i2c_client *i2c_client;
	struct mutex ops_mutex;
};

static struct adv7533_reg_cfg adv7533_init_setup[] = {
	{I2C_ADDR_MAIN, 0x41, 0x10},		/* HDMI normal */
	{I2C_ADDR_MAIN, 0xd6, 0x48},		/* HPD overriden */
	{I2C_ADDR_CEC_DSI, 0x03, 0x89},		/* HDMI enabled */
	{I2C_ADDR_MAIN, 0x16, 0x20},
	/* Fixed */
	{I2C_ADDR_MAIN, 0x9A, 0xE0},
	/* HDCP */
	{I2C_ADDR_MAIN, 0xBA, 0x70},
	/* Fixed */
	{I2C_ADDR_MAIN, 0xDE, 0x82},
	/* V1P2 */
	{I2C_ADDR_MAIN, 0xE4, 0x40},
	/* Fixed */
	{I2C_ADDR_MAIN, 0xE5, 0x80},
	/* Fixed */
	{I2C_ADDR_CEC_DSI, 0x15, 0xD0},
	/* Fixed */
	{I2C_ADDR_CEC_DSI, 0x17, 0xD0},
	/* Fixed */
	{I2C_ADDR_CEC_DSI, 0x24, 0x20},
	/* Fixed */
	{I2C_ADDR_CEC_DSI, 0x57, 0x11},


	/* Reset Internal Timing Generator */
	{I2C_ADDR_MAIN, 0xAF, 0x16},
	/* HDMI Mode Select */
	{I2C_ADDR_CEC_DSI, 0x78, 0x03}
	
};

static struct adv7533_reg_cfg adv7533_video_en[] = {
	 /* Timing Generator Enable */
	{I2C_ADDR_CEC_DSI, 0x27, 0xCB},
	{I2C_ADDR_CEC_DSI, 0x27, 0x8B},
	{I2C_ADDR_CEC_DSI, 0x27, 0xCB},
	/* power up */
	{I2C_ADDR_MAIN, 0x41, 0x10},
	/* hdmi enable */
	{I2C_ADDR_CEC_DSI, 0x03, 0x89},
	/* color depth */
	{I2C_ADDR_MAIN, 0x4C, 0x04},
	/* down dither */
	{I2C_ADDR_MAIN, 0x49, 0x02},
	/* Audio and CEC clock gate */
	{I2C_ADDR_CEC_DSI, 0x05, 0xC8},
	/* GC packet enable */
	{I2C_ADDR_MAIN, 0x40, 0x80}

};

static struct adv7533_reg_cfg adv7533_cec_en[] = {
	/* Fixed, clock gate disable */
	{I2C_ADDR_CEC_DSI, 0x05, 0xC8},
	/* read divider(7:2) from calc */
	{I2C_ADDR_CEC_DSI, 0xBE, 0x01}
	
};

static struct adv7533_reg_cfg adv7533_cec_tg_init[] = {
	/* TG programming for 19.2MHz, divider 25 */
	{I2C_ADDR_CEC_DSI, 0xBE, 0x61},
	{I2C_ADDR_CEC_DSI, 0xC1, 0x0D},
	{I2C_ADDR_CEC_DSI, 0xC2, 0x80},
	{I2C_ADDR_CEC_DSI, 0xC3, 0x0C},
	{I2C_ADDR_CEC_DSI, 0xC4, 0x9A},
	{I2C_ADDR_CEC_DSI, 0xC5, 0x0E},
	{I2C_ADDR_CEC_DSI, 0xC6, 0x66},
	{I2C_ADDR_CEC_DSI, 0xC7, 0x0B},
	{I2C_ADDR_CEC_DSI, 0xC8, 0x1A},
	{I2C_ADDR_CEC_DSI, 0xC9, 0x0A},
	{I2C_ADDR_CEC_DSI, 0xCA, 0x33},
	{I2C_ADDR_CEC_DSI, 0xCB, 0x0C},
	{I2C_ADDR_CEC_DSI, 0xCC, 0x00},
	{I2C_ADDR_CEC_DSI, 0xCD, 0x07},
	{I2C_ADDR_CEC_DSI, 0xCE, 0x33},
	{I2C_ADDR_CEC_DSI, 0xCF, 0x05},
	{I2C_ADDR_CEC_DSI, 0xD0, 0xDA},
	{I2C_ADDR_CEC_DSI, 0xD1, 0x08},
	{I2C_ADDR_CEC_DSI, 0xD2, 0x8D},
	{I2C_ADDR_CEC_DSI, 0xD3, 0x01},
	{I2C_ADDR_CEC_DSI, 0xD4, 0xCD},
	{I2C_ADDR_CEC_DSI, 0xD5, 0x04},
	{I2C_ADDR_CEC_DSI, 0xD6, 0x80},
	{I2C_ADDR_CEC_DSI, 0xD7, 0x05},
	{I2C_ADDR_CEC_DSI, 0xD8, 0x66},
	{I2C_ADDR_CEC_DSI, 0xD9, 0x03},
	{I2C_ADDR_CEC_DSI, 0xDA, 0x26},
	{I2C_ADDR_CEC_DSI, 0xDB, 0x0A},
	{I2C_ADDR_CEC_DSI, 0xDC, 0xCD},
	{I2C_ADDR_CEC_DSI, 0xDE, 0x00},
	{I2C_ADDR_CEC_DSI, 0xDF, 0xC0},
	{I2C_ADDR_CEC_DSI, 0xE1, 0x00},
	{I2C_ADDR_CEC_DSI, 0xE2, 0xE6},
	{I2C_ADDR_CEC_DSI, 0xE3, 0x02},
	{I2C_ADDR_CEC_DSI, 0xE4, 0xB3},
	{I2C_ADDR_CEC_DSI, 0xE5, 0x03},
	{I2C_ADDR_CEC_DSI, 0xE6, 0x9A}
	
};

static struct adv7533_reg_cfg adv7533_cec_power[] = {
	/* cec power up */
	{I2C_ADDR_MAIN, 0xE2, 0x00},
	/* hpd override */
	{I2C_ADDR_MAIN, 0xD6, 0x48},
	/* edid reread */
	{I2C_ADDR_MAIN, 0xC9, 0x13},
	/* read all CEC Rx Buffers */
	{I2C_ADDR_CEC_DSI, 0xBA, 0x08},
	/* logical address0 0x04 */
	{I2C_ADDR_CEC_DSI, 0xBC, 0x04},
	/* select logical address0 */
	{I2C_ADDR_CEC_DSI, 0xBB, 0x10}
	
};


static struct adv7533_reg_cfg I2S_cfg[] = {
	{I2C_ADDR_MAIN, 0x0D, 0x18},	/* Bit width = 16Bits*/
	{I2C_ADDR_MAIN, 0x15, 0x20},	/* Sampling Frequency = 48kHz*/
	{I2C_ADDR_MAIN, 0x02, 0x18},	/* N value 6144 --> 0x1800*/
	{I2C_ADDR_MAIN, 0x14, 0x02},	/* Word Length = 16Bits*/
	{I2C_ADDR_MAIN, 0x73, 0x01}	/* Channel Count = 2 channels */
	
};


static struct i2c_client *client;


/*
 * If i2c read or write fails, wait for 100ms to try again, and try
 * max 3 times.
 */
#define MAX_WAIT_TIME (100)
#define MAX_RW_TRIES (3)

static int adv7533_read(u8 addr, u8 reg, u8 *buf, u8 len)
{
	int ret = 0, i = 0;
	struct i2c_msg msg[2];

	if (!client) {
		pr_err("%s: no adv7533 i2c client\n", __func__);
		ret = -ENODEV;
		goto r_err;
	}

	if (NULL == buf) {
		pr_err("%s: no adv7533 i2c client\n", __func__);
		ret = -EINVAL;
		goto r_err;
	}

	client->addr = addr;

	msg[0].addr = addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &reg;

	msg[1].addr = addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = buf;

	do {
		if (i2c_transfer(client->adapter, msg, 2) == 2) {
			ret = 0;
			goto r_err;
		}
		msleep(MAX_WAIT_TIME);
	} while (++i < MAX_RW_TRIES);

	ret = -EIO;
	pr_err("%s adv7533 i2c read failed after %d tries\n", __func__,
		MAX_RW_TRIES);

r_err:
	return ret;
}

int adv7533_read_byte(u8 addr, u8 reg, u8 *buf)
{
	return adv7533_read(addr, reg, buf, 1);
}

static int adv7533_write_byte(u8 addr, u8 reg, u8 val)
{
	int ret = 0, i = 0;
	u8 buf[2] = {reg, val};
	struct i2c_msg msg[1];

	if (!client) {
		pr_err("%s: no adv7533 i2c client\n", __func__);
		ret = -ENODEV;
		goto w_err;
	}

	client->addr = addr;

	msg[0].addr = addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = buf;

	do {
		if (i2c_transfer(client->adapter, msg, 1) >= 1) {
			ret = 0;
			goto w_err;
		}
		msleep(MAX_WAIT_TIME);
	} while (++i < MAX_RW_TRIES);

	ret = -EIO;
	pr_err("%s: adv7533 i2c write failed after %d tries\n", __func__,
		MAX_RW_TRIES);

w_err:
	if (ret != 0)
		pr_err("%s: Exiting with ret = %d after %d retries\n",
			__func__, ret, i);
	return ret;
}


static int adv7533_write_regs(struct adv7533 *pdata,
	struct adv7533_reg_cfg *cfg, int size)
{
	int ret = 0;
	int i;

	for (i = 0; i < size; i++) {
		switch (cfg[i].i2c_addr) {
		case I2C_ADDR_MAIN:
			ret = adv7533_write_byte(pdata->main_i2c_addr,
				cfg[i].reg, cfg[i].val);
			if (ret != 0)
				pr_err("%s: adv7533_write_byte returned %d\n",
					__func__, ret);
			break;
		case I2C_ADDR_CEC_DSI:
			ret = adv7533_write_byte(pdata->cec_dsi_i2c_addr,
				cfg[i].reg, cfg[i].val);
			if (ret != 0)
				pr_err("%s: adv7533_write_byte returned %d\n",
					__func__, ret);
			break;
		default:
			ret = -EINVAL;
			pr_err("%s: Default case? BUG!\n", __func__);
			break;
		}
		if (ret != 0) {
			pr_err("%s: adv7533 reg writes failed. ", __func__);
			pr_err("Last write %02X to %02X\n",
				cfg[i].val, cfg[i].reg);
			goto w_regs_fail;
		}
	}

w_regs_fail:
	if (ret != 0)
		pr_err("%s: Exiting with ret = %d after %d writes\n",
			__func__, ret, i);
	return ret;
}


static int adv7533_read_device_rev(void)
{
	u8 rev = 0;
	int ret;

	ret = adv7533_read_byte(I2C_ADDR_MAIN, ADV7533_REG_CHIP_REVISION,
							&rev);

	if (!ret)
		pr_debug("%s: adv7533 revision 0x%X\n", __func__, rev);
	else
		pr_err("%s: adv7533 rev error\n", __func__);
	printk("%s: adv7533 revision 0x%X\n", __func__, rev);

	return ret;
}



static int adv7533_parse_dt(struct device *dev,
	struct adv7533 *pdata)
{
	struct device_node *np = dev->of_node;
	u32 temp_val;
	int ret = 0;

	ret = of_property_read_u32(np, "adv7533,main-addr", &temp_val);
	pr_debug("%s: DT property %s is %X\n", __func__, "adv7533,main-addr",
		temp_val);
	if (ret)
		goto end;
	pdata->main_i2c_addr = (u8)temp_val;

	ret = of_property_read_u32(np, "adv7533,cec-dsi-addr", &temp_val);
	pr_debug("%s: DT property %s is %X\n", __func__, "adv7533,cec-dsi-addr",
		temp_val);
	if (ret)
		goto end;
	pdata->cec_dsi_i2c_addr = (u8)temp_val;

	ret = of_property_read_u32(np, "adv7533,video-mode", &temp_val);
	pr_debug("%s: DT property %s is %X\n", __func__, "adv7533,video-mode",
		temp_val);
	if (ret)
		goto end;
	pdata->video_mode = (u8)temp_val;

	ret = of_property_read_u32(np, "adv7533,audio", &temp_val);
	pr_debug("%s: DT property %s is %X\n",
		__func__, "adv7533,audio", temp_val);
	if (ret)
		goto end;
	pdata->audio = (u8)temp_val;

	/* Get pinctrl if target uses pinctrl */
	pdata->ts_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(pdata->ts_pinctrl)) {
		ret = PTR_ERR(pdata->ts_pinctrl);
		pr_err("%s: Pincontrol DT property returned %X\n",
			__func__, ret);
	}

	pdata->pinctrl_state_active = pinctrl_lookup_state(pdata->ts_pinctrl,
		"pmx_adv7533_active");
	if (IS_ERR_OR_NULL(pdata->pinctrl_state_active)) {
		ret = PTR_ERR(pdata->pinctrl_state_active);
		pr_err("Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_ACTIVE, ret);
	}

	pdata->pinctrl_state_suspend = pinctrl_lookup_state(pdata->ts_pinctrl,
		"pmx_adv7533_suspend");
	if (IS_ERR_OR_NULL(pdata->pinctrl_state_suspend)) {
		ret = PTR_ERR(pdata->pinctrl_state_suspend);
		pr_err("Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_SUSPEND, ret);
	}

	pdata->disable_gpios = of_property_read_bool(np,
			"adv7533,disable-gpios");

	if (!(pdata->disable_gpios)) {
		pdata->irq_gpio = of_get_named_gpio_flags(np,
				"adv7533,irq-gpio", 0, &pdata->irq_flags);

		pdata->hpd_irq_gpio = of_get_named_gpio_flags(np,
				"adv7533,hpd-irq-gpio", 0,
				&pdata->hpd_irq_flags);

		pdata->switch_gpio = of_get_named_gpio_flags(np,
				"adv7533,switch-gpio", 0, &pdata->switch_flags);
	}

end:
	return ret;
}

static int adv7533_config_clocks(struct adv7533 *pdata, int enable)
{
	int rc = 0;
	struct dss_module_power *mp;

	if (!pdata) {
		pr_err("%s: invalid input\n", __func__);
		rc = -EINVAL;
		goto end;
	}

	mp = &pdata->power_data;
	if (!mp) {
		pr_err("%s: invalid power data\n", __func__);
		rc = -EINVAL;
		goto end;
	}

	if (enable) {
		rc = msm_dss_clk_set_rate(mp->clk_config, mp->num_clk);
		if (rc) {
			pr_err("%s: Failed to set clock rate rc=%d\n",
				__func__, rc);
			goto end;
		}

		rc = msm_dss_enable_clk(mp->clk_config, mp->num_clk, enable);
		if (rc) {
			pr_err("%s: clock enable failed rc:%d\n", __func__, rc);
			goto end;
		}
	} else {
		msm_dss_enable_clk(mp->clk_config, mp->num_clk, enable);
	}
end:
	return rc;
}

static int adv7533_gpio_configure(struct adv7533 *pdata, bool on)
{
	int ret = 0;

	if (pdata->disable_gpios)
		return 0;

	if (on) {
		if (gpio_is_valid(pdata->irq_gpio)) {
			ret = gpio_request(pdata->irq_gpio, "adv7533_irq_gpio");
			if (ret) {
				pr_err("unable to request gpio [%d]\n",
					pdata->irq_gpio);
				goto err_none;
			}
			ret = gpio_direction_input(pdata->irq_gpio);
			if (ret) {
				pr_err("unable to set dir for gpio[%d]\n",
					pdata->irq_gpio);
				goto err_irq_gpio;
			}
		} else {
			pr_err("irq gpio not provided\n");
			goto err_none;
		}

		if (gpio_is_valid(pdata->hpd_irq_gpio)) {
			ret = gpio_request(pdata->hpd_irq_gpio,
				"adv7533_hpd_irq_gpio");
			if (ret) {
				pr_err("unable to request gpio [%d]\n",
					pdata->hpd_irq_gpio);
				goto err_irq_gpio;
			}
			ret = gpio_direction_input(pdata->hpd_irq_gpio);
			if (ret) {
				pr_err("unable to set dir for gpio[%d]\n",
					pdata->hpd_irq_gpio);
				goto err_hpd_irq_gpio;
			}
		} else {
			pr_err("hpd irq gpio not provided\n");
			goto err_irq_gpio;
		}

		if (gpio_is_valid(pdata->switch_gpio)) {
			ret = gpio_request(pdata->switch_gpio,
				"adv7533_switch_gpio");
			if (ret) {
				pr_err("unable to request gpio [%d]\n",
					pdata->switch_gpio);
				goto err_hpd_irq_gpio;
			}

			ret = gpio_direction_output(pdata->switch_gpio, 1);
			if (ret) {
				pr_err("unable to set dir for gpio [%d]\n",
					pdata->switch_gpio);
				goto err_switch_gpio;
			}

			gpio_set_value(pdata->switch_gpio, 1);
			msleep(ADV7533_RESET_DELAY);
		}

		return 0;
	} else {
		if (gpio_is_valid(pdata->irq_gpio))
			gpio_free(pdata->irq_gpio);
		if (gpio_is_valid(pdata->hpd_irq_gpio))
			gpio_free(pdata->hpd_irq_gpio);
		if (gpio_is_valid(pdata->switch_gpio))
			gpio_free(pdata->switch_gpio);

		return 0;
	}

err_switch_gpio:
	if (gpio_is_valid(pdata->switch_gpio))
		gpio_free(pdata->switch_gpio);
err_hpd_irq_gpio:
	if (gpio_is_valid(pdata->hpd_irq_gpio))
		gpio_free(pdata->hpd_irq_gpio);
err_irq_gpio:
	if (gpio_is_valid(pdata->irq_gpio))
		gpio_free(pdata->irq_gpio);
err_none:
	return ret;
}

static void adv7533_notify_clients(struct msm_dba_device_info *dev,
		enum msm_dba_callback_event event)
{
	struct msm_dba_client_info *c;
	struct list_head *pos = NULL;

	if (!dev) {
		pr_err("%s: invalid input\n", __func__);
		return;
	}

	list_for_each(pos, &dev->client_list) {
		c = list_entry(pos, struct msm_dba_client_info, list);

		pr_debug("%s: notifying event %d to client %s\n", __func__,
			event, c->client_name);

		if (c && c->cb)
			c->cb(c->cb_data, event);
	}
}

u32 adv7533_read_edid(struct adv7533 *pdata, u32 size, char *edid_buf)
{
	u32 ret = 0, read_size = size / 2;
	u8 edid_addr;

	if (!pdata || !edid_buf)
		return 0;

	pr_debug("%s: size %d\n", __func__, size);

	adv7533_read(I2C_ADDR_MAIN, 0x43, &edid_addr, 1);

	pr_debug("%s: edid address 0x%x\n", __func__, edid_addr);

	adv7533_read(edid_addr >> 1, 0x00, edid_buf, read_size);

	adv7533_read(edid_addr >> 1, read_size,
		edid_buf + read_size, read_size);
	return ret;
}

static int adv7533_cec_prepare_msg(struct adv7533 *pdata, u8 *msg, u32 size)
{
	int i, ret = -EINVAL;
	int op_sz;

	if (!pdata || !msg) {
		pr_err("%s: invalid input\n", __func__);
		goto end;
	}

	if (size <= 0 || size > CEC_MSG_SIZE) {
		pr_err("%s: ERROR: invalid msg size\n", __func__);
		goto end;
	}

	/* operand size = total size - header size - opcode size */
	op_sz = size - 2;

	/* write header */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x70, msg[0]);

	/* write opcode */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x71, msg[1]);

	/* write operands */
	for (i = 0; i < op_sz && i < MAX_OPERAND_SIZE; i++) {
		pr_debug("%s: writing operands\n", __func__);
		adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x72 + i, msg[i + 2]);
	}

	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x80, size);

end:
	return ret;
}

static int adv7533_rd_cec_msg(struct adv7533 *pdata, u8 *cec_buf, int msg_num)
{
	int ret = -EINVAL;
	u8 reg = 0;

	if (!pdata || !cec_buf) {
		pr_err("%s: Invalid input\n", __func__);
		goto end;
	}

	if (msg_num == ADV7533_CEC_BUF1)
		reg = 0x85;
	else if (msg_num == ADV7533_CEC_BUF2)
		reg = 0x97;
	else if (msg_num == ADV7533_CEC_BUF3)
		reg = 0xA8;
	else
		pr_err("%s: Invalid msg_num %d\n", __func__, msg_num);

	if (!reg)
		goto end;

	adv7533_read(I2C_ADDR_CEC_DSI, reg, cec_buf, CEC_MSG_SIZE);
end:
	return ret;
}

static void adv7533_handle_hdcp_intr(struct adv7533 *pdata, u8 hdcp_status)
{

	if (!pdata) {
		pr_err("%s: Invalid input\n", __func__);
		goto end;
	}

	/* HDCP ready for read */
	if (hdcp_status & BIT(6))
		pr_debug("%s: BKSV FLAG\n", __func__);

	/* check for HDCP error */
	if (hdcp_status & BIT(7)) {
		u8 ddc_status;
		pr_err("%s: HDCP ERROR\n", __func__);

		/* get error details */
		adv7533_read(I2C_ADDR_MAIN, 0xC8, &ddc_status, 1);

		switch (ddc_status & 0xF0 >> 4) {
		case 0:
			pr_debug("%s: DDC: NO ERROR\n", __func__);
			break;
		case 1:
			pr_err("%s: DDC: BAD RX BKSV\n", __func__);
			break;
		case 2:
			pr_err("%s: DDC: Ri MISMATCH\n", __func__);
			break;
		case 3:
			pr_err("%s: DDC: Pj MISMATCH\n", __func__);
			break;
		case 4:
			pr_err("%s: DDC: I2C ERROR\n", __func__);
			break;
		case 5:
			pr_err("%s: DDC: TIMED OUT DS DONE\n", __func__);
			break;
		case 6:
			pr_err("%s: DDC: MAX CAS EXC\n", __func__);
			break;
		default:
			pr_debug("%s: DDC: UNKNOWN ERROR\n", __func__);
		}
	}
end:
	return;
}

static void adv7533_handle_cec_intr(struct adv7533 *pdata, u8 cec_status)
{
	u8 cec_int_clear = 0x08;
	bool cec_rx_intr = false;
	u8 cec_rx_ready;
	u8 cec_rx_timestamp;

	if (!pdata) {
		pr_err("%s: Invalid input\n", __func__);
		goto end;
	}

	if (cec_status & 0x07) {
		cec_rx_intr = true;
		adv7533_read(I2C_ADDR_CEC_DSI, 0xBA, &cec_int_clear, 1);
	}

	if (cec_status & BIT(5))
		pr_debug("%s: CEC TX READY\n", __func__);

	if (cec_status & BIT(4))
		pr_debug("%s: CEC TX Arbitration lost\n", __func__);

	if (cec_status & BIT(3))
		pr_debug("%s: CEC TX retry timout\n", __func__);

	if (!cec_rx_intr)
		return;


	adv7533_read(I2C_ADDR_CEC_DSI, 0xB9, &cec_rx_ready, 1);

	adv7533_read(I2C_ADDR_CEC_DSI, 0x96, &cec_rx_timestamp, 1);

	if (cec_rx_ready & BIT(0)) {
		pr_debug("%s: CEC Rx buffer 1 ready\n", __func__);
		adv7533_rd_cec_msg(pdata,
			pdata->cec_msg[ADV7533_CEC_BUF1].buf,
			ADV7533_CEC_BUF1);

		pdata->cec_msg[ADV7533_CEC_BUF1].pending = true;

		pdata->cec_msg[ADV7533_CEC_BUF1].timestamp =
			cec_rx_timestamp & (BIT(0) | BIT(1));

		adv7533_notify_clients(&pdata->dev_info,
			MSM_DBA_CB_CEC_READ_PENDING);
	}

	if (cec_rx_ready & BIT(1)) {
		pr_debug("%s: CEC Rx buffer 2 ready\n", __func__);
		adv7533_rd_cec_msg(pdata,
			pdata->cec_msg[ADV7533_CEC_BUF2].buf,
			ADV7533_CEC_BUF2);

		pdata->cec_msg[ADV7533_CEC_BUF2].pending = true;

		pdata->cec_msg[ADV7533_CEC_BUF2].timestamp =
			cec_rx_timestamp & (BIT(2) | BIT(3));

		adv7533_notify_clients(&pdata->dev_info,
			MSM_DBA_CB_CEC_READ_PENDING);
	}

	if (cec_rx_ready & BIT(2)) {
		pr_debug("%s: CEC Rx buffer 3 ready\n", __func__);
		adv7533_rd_cec_msg(pdata,
			pdata->cec_msg[ADV7533_CEC_BUF3].buf,
			ADV7533_CEC_BUF3);

		pdata->cec_msg[ADV7533_CEC_BUF3].pending = true;

		pdata->cec_msg[ADV7533_CEC_BUF3].timestamp =
			cec_rx_timestamp & (BIT(4) | BIT(5));

		adv7533_notify_clients(&pdata->dev_info,
			MSM_DBA_CB_CEC_READ_PENDING);
	}

	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0xBA,
		cec_int_clear | (cec_status & 0x07));

	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0xBA, cec_int_clear & ~0x07);

end:
	return;
}

static int adv7533_edid_read_init(struct adv7533 *pdata)
{
	int ret = -EINVAL;

	if (!pdata) {
		pr_err("%s: invalid pdata\n", __func__);
		goto end;
	}

	/* initiate edid read in adv7533 */
	adv7533_write_byte(I2C_ADDR_MAIN, 0x41, 0x10);
	adv7533_write_byte(I2C_ADDR_MAIN, 0xC9, 0x13);

end:
	return ret;
}

static void *adv7533_handle_hpd_intr(struct adv7533 *pdata)
{
	int ret;
	u8 hpd_state;
	u8 connected = 0, disconnected = 0;

	if (!pdata) {
		pr_err("%s: invalid pdata\n", __func__);
		goto end;
	}

	adv7533_read(I2C_ADDR_MAIN, 0x42, &hpd_state, 1);

	connected = (hpd_state & BIT(5)) && (hpd_state & BIT(6));
	disconnected = !(hpd_state & (BIT(5) | BIT(6)));

	if (connected) {
		pr_debug("%s: Rx CONNECTED\n", __func__);
	} else if (disconnected) {
		pr_debug("%s: Rx DISCONNECTED\n", __func__);

		adv7533_notify_clients(&pdata->dev_info,
			MSM_DBA_CB_HPD_DISCONNECT);
	} else {
		pr_debug("%s: HPD Intermediate state\n", __func__);
	}

	ret = connected ? 1 : 0;
end:
	return ERR_PTR(ret);
}

static int adv7533_enable_interrupts(struct adv7533 *pdata, int interrupts)
{
	int ret = 0;
	u8 reg_val, init_reg_val;

	if (!pdata) {
		pr_err("%s: invalid input\n", __func__);
		goto end;
	}

		adv7533_read(I2C_ADDR_MAIN, 0x94, &reg_val, 1);

	init_reg_val = reg_val;

	if (interrupts & CFG_HPD_INTERRUPTS)
		reg_val |= HPD_INTERRUPTS;

	if (interrupts & CFG_EDID_INTERRUPTS)
		reg_val |= EDID_INTERRUPTS;

	if (interrupts & CFG_HDCP_INTERRUPTS)
		reg_val |= HDCP_INTERRUPTS1;

	if (reg_val != init_reg_val) {
		pr_debug("%s: enabling 0x94 interrupts\n", __func__);
		
		adv7533_write_byte(I2C_ADDR_MAIN, 0x94, reg_val);
	}

	
	adv7533_read(I2C_ADDR_MAIN, 0x95, &reg_val, 1);

	init_reg_val = reg_val;

	if (interrupts & CFG_HDCP_INTERRUPTS)
		reg_val |= HDCP_INTERRUPTS2;

	if (interrupts & CFG_CEC_INTERRUPTS)
		reg_val |= CEC_INTERRUPTS;

	if (reg_val != init_reg_val) {
		pr_debug("%s: enabling 0x95 interrupts\n", __func__);
		
		adv7533_write_byte(I2C_ADDR_MAIN, 0x95, reg_val);
	}
end:
	return ret;
}

static int adv7533_disable_interrupts(struct adv7533 *pdata, int interrupts)
{
	int ret = 0;
	u8 reg_val, init_reg_val;

	if (!pdata) {
		pr_err("%s: invalid input\n", __func__);
		goto end;
	}

	adv7533_read(I2C_ADDR_MAIN, 0x94, &reg_val, 1);

	init_reg_val = reg_val;

	if (interrupts & CFG_HPD_INTERRUPTS)
		reg_val &= ~HPD_INTERRUPTS;

	if (interrupts & CFG_EDID_INTERRUPTS)
		reg_val &= ~EDID_INTERRUPTS;

	if (interrupts & CFG_HDCP_INTERRUPTS)
		reg_val &= ~HDCP_INTERRUPTS1;

	if (reg_val != init_reg_val) {
		pr_debug("%s: disabling 0x94 interrupts\n", __func__);
		adv7533_write_byte(I2C_ADDR_MAIN, 0x94, reg_val);
	}

	adv7533_read(I2C_ADDR_MAIN, 0x95, &reg_val, 1);

	init_reg_val = reg_val;

	if (interrupts & CFG_HDCP_INTERRUPTS)
		reg_val &= ~HDCP_INTERRUPTS2;

	if (interrupts & CFG_CEC_INTERRUPTS)
		reg_val &= ~CEC_INTERRUPTS;

	if (reg_val != init_reg_val) {
		pr_debug("%s: disabling 0x95 interrupts\n", __func__);
		adv7533_write_byte(I2C_ADDR_MAIN, 0x95, reg_val);
	}
end:
	return ret;
}

static void adv7533_intr_work(struct work_struct *work)
{
	int ret;
	u8 int_status  = 0xFF;
	u8 hdcp_cec_status = 0xFF;
	u32 interrupts = 0;
	int connected = false;
	struct adv7533 *pdata;
	struct delayed_work *dw = to_delayed_work(work);



        pdata = container_of(dw, struct adv7533,
                             adv7533_intr_work_id);
	if (!pdata) {
		pr_err("%s: invalid input\n", __func__);
		goto reset;
	}

	/* READ Interrupt registers */
	adv7533_read(I2C_ADDR_MAIN, 0x96, &int_status, 1);
	adv7533_read(I2C_ADDR_MAIN, 0x97, &hdcp_cec_status, 1);

	

	if (int_status & (BIT(6) | BIT(7))) {
		void *ptr_val = adv7533_handle_hpd_intr(pdata);

		ret = PTR_ERR(ptr_val);
		if (IS_ERR(ptr_val)) {
			pr_err("%s: error in hpd handing: %d\n",
				__func__, ret);
			goto reset;
		}
		connected = ret;
	}


	/* EDID ready for read */
	if (int_status & BIT(2)) {
		pr_debug("%s: EDID READY\n", __func__);

		ret = adv7533_read_edid(pdata, sizeof(pdata->edid_buf),
			pdata->edid_buf);

		if (ret)
			pr_err("%s: edid read failed\n", __func__);

		adv7533_notify_clients(&pdata->dev_info,
			MSM_DBA_CB_HPD_CONNECT); 
	}

	if (pdata->hdcp_enabled)
		adv7533_handle_hdcp_intr(pdata, hdcp_cec_status);

	if (pdata->cec_enabled)
		adv7533_handle_cec_intr(pdata, hdcp_cec_status);
reset:
	/* Clear HPD/EDID interrupts */
	adv7533_write_byte(I2C_ADDR_MAIN, 0x96, int_status);

	/* Clear HDCP/CEC interrupts */
	adv7533_write_byte(I2C_ADDR_MAIN, 0x97, hdcp_cec_status);

	/* Re-enable HPD interrupts */
	interrupts |= CFG_HPD_INTERRUPTS;

	/* Re-enable EDID interrupts */
	interrupts |= CFG_EDID_INTERRUPTS;

	/* Re-enable HDCP interrupts */
	if (pdata->hdcp_enabled)
		interrupts |= CFG_HDCP_INTERRUPTS;

	/* Re-enable CEC interrupts */
	if (pdata->cec_enabled)
		interrupts |= CFG_CEC_INTERRUPTS;

	if (adv7533_enable_interrupts(pdata, interrupts))
		pr_err("%s: err enabling interrupts\n", __func__);

	/* initialize EDID read after cable connected */
	if (connected)
		adv7533_edid_read_init(pdata);
}

static irqreturn_t adv7533_irq(int irq, void *data)
{
	struct adv7533 *pdata = data;
	u32 interrupts = 0;

	if (!pdata) {
		pr_err("%s: invalid input\n", __func__);
		return IRQ_HANDLED;
	}

	/* disable HPD interrupts */
	interrupts |= CFG_HPD_INTERRUPTS;

	/* disable EDID interrupts */
	interrupts |= CFG_EDID_INTERRUPTS;

	/* disable HDCP interrupts */
	if (pdata->hdcp_enabled)
		interrupts |= CFG_HDCP_INTERRUPTS;

	/* disable CEC interrupts */
	if (pdata->cec_enabled)
		interrupts |= CFG_CEC_INTERRUPTS;

	if (adv7533_disable_interrupts(pdata, interrupts))
		pr_err("%s: err disabling interrupts\n", __func__);

	queue_delayed_work(pdata->workq, &pdata->adv7533_intr_work_id, 0);

	return IRQ_HANDLED;
}

static struct i2c_device_id adv7533_id[] = {
	{ "adv7533", 0},
	{}
};

static struct adv7533 *adv7533_get_platform_data(void *client)
{
	struct adv7533 *pdata = NULL;
	struct msm_dba_device_info *dev;
	struct msm_dba_client_info *cinfo =
		(struct msm_dba_client_info *)client;

	if (!cinfo) {
		pr_err("%s: invalid client data\n", __func__);
		goto end;
	}

	dev = cinfo->dev;
	if (!dev) {
		pr_err("%s: invalid device data\n", __func__);
		goto end;
	}

	pdata = container_of(dev, struct adv7533, dev_info);
	if (!pdata)
		pr_err("%s: invalid platform data\n", __func__);

end:
	return pdata;
}

static int adv7533_cec_enable(void *client, bool cec_on, u32 flags)
{
	int ret = -EINVAL;
	struct adv7533 *pdata = adv7533_get_platform_data(client);

	if (!pdata) {
		pr_err("%s: invalid platform data\n", __func__);
		goto end;
	}

	if (cec_on) {
		adv7533_write_regs( pdata, adv7533_cec_en, ARRAY_SIZE(adv7533_cec_en));
		adv7533_write_regs( pdata,adv7533_cec_tg_init, ARRAY_SIZE(adv7533_cec_tg_init));
		adv7533_write_regs( pdata,adv7533_cec_power, ARRAY_SIZE(adv7533_cec_power));

		pdata->cec_enabled = true;

		ret = adv7533_enable_interrupts(pdata, CFG_CEC_INTERRUPTS);

	} else {
		pdata->cec_enabled = false;
		ret = adv7533_disable_interrupts(pdata, CFG_CEC_INTERRUPTS);
	}
end:
	return ret;
}

static int adv7533_check_hpd(void *client, u32 flags)
{
	int ret = -EINVAL;
	struct adv7533 *pdata = adv7533_get_platform_data(client);
	u8 reg_val = 0;
	u8 intr_status;
	int connected = 0;

	if (!pdata) {
		pr_err("%s: invalid platform data\n", __func__);
		return ret;
	}

	/* Check if cable is already connected.
	 * Since adv7533_irq line is edge triggered,
	 * if cable is already connected by this time
	 * it won't trigger HPD interrupt.
	 */
	mutex_lock(&pdata->ops_mutex);
	adv7533_read(I2C_ADDR_MAIN, 0x42, &reg_val, 1);

	connected  = (reg_val & BIT(6));
	if (connected) {
		pr_debug("%s: cable is connected\n", __func__);
		/* Clear the interrupts before initiating EDID read */
		adv7533_read(I2C_ADDR_MAIN, 0x96, &intr_status, 1);
		adv7533_write_byte(I2C_ADDR_MAIN, 0x96, intr_status);
		adv7533_enable_interrupts(pdata, (CFG_EDID_INTERRUPTS |
				CFG_HPD_INTERRUPTS));

		adv7533_edid_read_init(pdata);
	}

	mutex_unlock(&pdata->ops_mutex);
	return connected;
}

/* Device Operations */
static int adv7533_power_on(void *client, bool on, u32 flags)
{
	int ret = -EINVAL;
	struct adv7533 *pdata = adv7533_get_platform_data(client);

	if (!pdata) {
		pr_err("%s: invalid platform data\n", __func__);
		return ret;
	}

	pr_debug("%s: %d\n", __func__, on);
	mutex_lock(&pdata->ops_mutex);

	if (on && !pdata->is_power_on) {
		adv7533_write_regs(pdata, adv7533_init_setup, ARRAY_SIZE(adv7533_init_setup));

		ret = adv7533_enable_interrupts(pdata, CFG_HPD_INTERRUPTS);
		if (ret) {
			pr_err("%s: Failed: enable HPD intr %d\n",
				__func__, ret);
			goto end;
		}
		pdata->is_power_on = true;
	} else if (!on) {
		/* power down hdmi */
		adv7533_write_byte(I2C_ADDR_MAIN, 0x41, 0x50);
		pdata->is_power_on = false;

		adv7533_notify_clients(&pdata->dev_info,
			MSM_DBA_CB_HPD_DISCONNECT);
	}
end:
	mutex_unlock(&pdata->ops_mutex);
	return ret;
}

static void adv7533_video_setup(struct adv7533 *pdata,
	struct msm_dba_video_cfg *cfg)
{
	u32 h_total, hpw, hfp, hbp;
	u32 v_total, vpw, vfp, vbp;

	if (!pdata || !cfg) {
		pr_err("%s: invalid input\n", __func__);
		return;
	}

	h_total = cfg->h_active + cfg->h_front_porch +
	      cfg->h_pulse_width + cfg->h_back_porch;
	v_total = cfg->v_active + cfg->v_front_porch +
	      cfg->v_pulse_width + cfg->v_back_porch;

	hpw = cfg->h_pulse_width;
	hfp = cfg->h_front_porch;
	hbp = cfg->h_back_porch;

	vpw = cfg->v_pulse_width;
	vfp = cfg->v_front_porch;
	vbp = cfg->v_back_porch;

	pr_debug("h_total 0x%x, h_active 0x%x, hfp 0x%d, hpw 0x%x, hbp 0x%x\n",
		h_total, cfg->h_active, cfg->h_front_porch,
		cfg->h_pulse_width, cfg->h_back_porch);

	pr_debug("v_total 0x%x, v_active 0x%x, vfp 0x%x, vpw 0x%x, vbp 0x%x\n",
		v_total, cfg->v_active, cfg->v_front_porch,
		cfg->v_pulse_width, cfg->v_back_porch);


	/* h_width */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x28, ((h_total & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x29, ((h_total & 0xF) << 4));

	/* hsync_width */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x2A, ((hpw & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x2B, ((hpw & 0xF) << 4));

	/* hfp */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x2C, ((hfp & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x2D, ((hfp & 0xF) << 4));

	/* hbp */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x2E, ((hbp & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x2F, ((hbp & 0xF) << 4));

	/* v_total */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x30, ((v_total & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x31, ((v_total & 0xF) << 4));

	/* vsync_width */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x32, ((vpw & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x33, ((vpw & 0xF) << 4));

	/* vfp */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x34, ((vfp & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x35, ((vfp & 0xF) << 4));

	/* vbp */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x36, ((vbp & 0xFF0) >> 4));
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x37, ((vbp & 0xF) << 4));
	return;
}

static int adv7533_video_on(void *client, bool on,
	struct msm_dba_video_cfg *cfg, u32 flags)
{
	int ret = -EINVAL;
	u8 lanes;
	u8 reg_val = 0;
	struct adv7533 *pdata = adv7533_get_platform_data(client);

	if (!pdata || !cfg) {
		pr_err("%s: invalid platform data\n", __func__);
		return ret;
	}

	mutex_lock(&pdata->ops_mutex);

	/* DSI lane configuration */
	lanes = (cfg->num_of_input_lanes << 4);
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x1C, lanes);

	adv7533_video_setup(pdata, cfg);

	/* hdmi/dvi mode */
	if (cfg->hdmi_mode)
		adv7533_write_byte(I2C_ADDR_MAIN, 0xAF, 0x06);

	/* set scan info for AVI Infoframe*/
	if (cfg->scaninfo) {
		adv7533_read(I2C_ADDR_MAIN, 0x55, &reg_val, 1);
		reg_val |= cfg->scaninfo & (BIT(1) | BIT(0));
		adv7533_write_byte(I2C_ADDR_MAIN, 0x55, reg_val);
	}

	/*
	 * aspect ratio and sync polarity set up.
	 * Currently adv only supports 16:9 or 4:3 aspect ratio
	 * configuration.
	 */
	if (cfg->h_active * 3 - cfg->v_active * 4) {
		adv7533_write_byte(I2C_ADDR_MAIN, 0x17, 0x02);
		adv7533_write_byte(I2C_ADDR_MAIN, 0x56, 0x28);
	} else {
		/* 4:3 aspect ratio */
		adv7533_write_byte(I2C_ADDR_MAIN, 0x17, 0x00);
		adv7533_write_byte(I2C_ADDR_MAIN, 0x56, 0x18);
	}

	adv7533_write_regs(pdata, adv7533_video_en, ARRAY_SIZE(adv7533_video_en));
	mutex_unlock(&pdata->ops_mutex);
	return ret;
}

static int adv7533_hdcp_enable(void *client, bool hdcp_on,
	bool enc_on, u32 flags)
{
	int ret = -EINVAL;
	u8 reg_val;
	struct adv7533 *pdata =
		adv7533_get_platform_data(client);

	if (!pdata) {
		pr_err("%s: invalid platform data\n", __func__);
		return ret;
	}

	mutex_lock(&pdata->ops_mutex);

	adv7533_read(I2C_ADDR_MAIN, 0xAF, &reg_val, 1);

	if (hdcp_on)
		reg_val |= BIT(7);
	else
		reg_val &= ~BIT(7);

	if (enc_on)
		reg_val |= BIT(4);
	else
		reg_val &= ~BIT(4);

	adv7533_write_byte(I2C_ADDR_MAIN, 0xAF, reg_val);

	pdata->hdcp_enabled = hdcp_on;

	if (pdata->hdcp_enabled)
		adv7533_enable_interrupts(pdata, CFG_HDCP_INTERRUPTS);
	else
		adv7533_disable_interrupts(pdata, CFG_HDCP_INTERRUPTS);
	mutex_unlock(&pdata->ops_mutex);
	return ret;
}

static int adv7533_configure_audio(void *client,
	struct msm_dba_audio_cfg *cfg, u32 flags)
{
	int ret = -EINVAL;
	int sampling_rate = 0;
	struct adv7533 *pdata =
		adv7533_get_platform_data(client);
	struct adv7533_reg_cfg reg_cfg[] = {
		{I2C_ADDR_MAIN, 0x12, 0x00},
		{I2C_ADDR_MAIN, 0x13, 0x00},
		{I2C_ADDR_MAIN, 0x14, 0x00},
		{I2C_ADDR_MAIN, 0x15, 0x00},
		{I2C_ADDR_MAIN, 0x0A, 0x00},
		{I2C_ADDR_MAIN, 0x0C, 0x00},
		{I2C_ADDR_MAIN, 0x0D, 0x00},
		{I2C_ADDR_MAIN, 0x03, 0x00},
		{I2C_ADDR_MAIN, 0x02, 0x00},
		{I2C_ADDR_MAIN, 0x01, 0x00},
		{I2C_ADDR_MAIN, 0x09, 0x00},
		{I2C_ADDR_MAIN, 0x08, 0x00},
		{I2C_ADDR_MAIN, 0x07, 0x00},
		{I2C_ADDR_MAIN, 0x73, 0x00},
		{I2C_ADDR_MAIN, 0x76, 0x00}
		
	};

	if (!pdata || !cfg) {
		pr_err("%s: invalid data\n", __func__);
		return ret;
	}

	mutex_lock(&pdata->ops_mutex);

	if (cfg->copyright == MSM_DBA_AUDIO_COPYRIGHT_NOT_PROTECTED)
		reg_cfg[0].val |= BIT(5);

	if (cfg->pre_emphasis == MSM_DBA_AUDIO_PRE_EMPHASIS_50_15us)
		reg_cfg[0].val |= BIT(2);

	if (cfg->clock_accuracy == MSM_DBA_AUDIO_CLOCK_ACCURACY_LVL1)
		reg_cfg[0].val |= BIT(0);
	else if (cfg->clock_accuracy == MSM_DBA_AUDIO_CLOCK_ACCURACY_LVL3)
		reg_cfg[0].val |= BIT(1);

	reg_cfg[1].val = cfg->channel_status_category_code;

	reg_cfg[2].val = (cfg->channel_status_word_length & 0xF) << 0 |
		(cfg->channel_status_source_number & 0xF) << 4;

	if (cfg->sampling_rate == MSM_DBA_AUDIO_32KHZ)
		sampling_rate = 0x3;
	else if (cfg->sampling_rate == MSM_DBA_AUDIO_44P1KHZ)
		sampling_rate = 0x0;
	else if (cfg->sampling_rate == MSM_DBA_AUDIO_48KHZ)
		sampling_rate = 0x2;
	else if (cfg->sampling_rate == MSM_DBA_AUDIO_88P2KHZ)
		sampling_rate = 0x8;
	else if (cfg->sampling_rate == MSM_DBA_AUDIO_96KHZ)
		sampling_rate = 0xA;
	else if (cfg->sampling_rate == MSM_DBA_AUDIO_176P4KHZ)
		sampling_rate = 0xC;
	else if (cfg->sampling_rate == MSM_DBA_AUDIO_192KHZ)
		sampling_rate = 0xE;

	reg_cfg[3].val = (sampling_rate & 0xF) << 4;

	if (cfg->mode == MSM_DBA_AUDIO_MODE_MANUAL)
		reg_cfg[4].val |= BIT(7);

	if (cfg->interface == MSM_DBA_AUDIO_SPDIF_INTERFACE)
		reg_cfg[4].val |= BIT(4);

	if (cfg->interface == MSM_DBA_AUDIO_I2S_INTERFACE) {
		/* i2s enable */
		reg_cfg[5].val |= BIT(2);

		/* audio samp freq select */
		reg_cfg[5].val |= BIT(7);
	}

	/* format */
	reg_cfg[5].val |= cfg->i2s_fmt & 0x3;

	/* channel status override */
	reg_cfg[5].val |= (cfg->channel_status_source & 0x1) << 6;

	/* sample word lengths, default 24 */
	reg_cfg[6].val |= 0x18;

	/* endian order of incoming I2S data */
	if (cfg->word_endianness == MSM_DBA_AUDIO_WORD_LITTLE_ENDIAN)
		reg_cfg[6].val |= 0x1 << 7;

	/* compressed audio v - bit */
	reg_cfg[6].val |= (cfg->channel_status_v_bit & 0x1) << 5;

	/* ACR - N */
	reg_cfg[7].val |= (cfg->n & 0x000FF) >> 0;
	reg_cfg[8].val |= (cfg->n & 0x0FF00) >> 8;
	reg_cfg[9].val |= (cfg->n & 0xF0000) >> 16;

	/* ACR - CTS */
	reg_cfg[10].val |= (cfg->cts & 0x000FF) >> 0;
	reg_cfg[11].val |= (cfg->cts & 0x0FF00) >> 8;
	reg_cfg[12].val |= (cfg->cts & 0xF0000) >> 16;

	/* channel count */
	reg_cfg[13].val |= (cfg->channels & 0x3);

	/* CA */
	reg_cfg[14].val = cfg->channel_allocation;

	adv7533_write_regs(pdata, reg_cfg, ARRAY_SIZE(reg_cfg));
	
	mutex_unlock(&pdata->ops_mutex);
	return ret;
}

static int adv7533_hdmi_cec_write(void *client, u32 size,
	char *buf, u32 flags)
{
	int ret = -EINVAL;
	struct adv7533 *pdata =
		adv7533_get_platform_data(client);

	if (!pdata) {
		pr_err("%s: invalid platform data\n", __func__);
		return ret;
	}

	mutex_lock(&pdata->ops_mutex);

	ret = adv7533_cec_prepare_msg(pdata, buf, size);
	if (ret)
		goto end;

	/* Enable CEC msg tx with NACK 3 retries */
	adv7533_write_byte(I2C_ADDR_CEC_DSI, 0x81, 0x07);
end:
	mutex_unlock(&pdata->ops_mutex);
	return ret;
}

static int adv7533_hdmi_cec_read(void *client, u32 *size, char *buf, u32 flags)
{
	int ret = -EINVAL;
	int i;
	struct adv7533 *pdata =
		adv7533_get_platform_data(client);

	if (!pdata) {
		pr_err("%s: invalid platform data\n", __func__);
		return ret;
	}

	mutex_lock(&pdata->ops_mutex);

	for (i = 0; i < ADV7533_CEC_BUF_MAX; i++) {
		struct adv7533_cec_msg *msg = &pdata->cec_msg[i];

		if (msg->pending && msg->timestamp) {
			memcpy(buf, msg->buf, CEC_MSG_SIZE);
			msg->pending = false;
			break;
		}
	}

	if (i < ADV7533_CEC_BUF_MAX) {
		*size = CEC_MSG_SIZE;
		ret = 0;
	} else {
		pr_err("%s: no pending cec msg\n", __func__);
		*size = 0;
	}

	mutex_unlock(&pdata->ops_mutex);
	return ret;
}

static int adv7533_get_edid_size(void *client, u32 *size, u32 flags)
{
	int ret = 0;
	struct adv7533 *pdata =
		adv7533_get_platform_data(client);

	if (!pdata) {
		pr_err("%s: invalid platform data\n", __func__);
		return ret;
	}

	mutex_lock(&pdata->ops_mutex);

	if (!size) {
		ret = -EINVAL;
		goto end;
	}

	*size = EDID_SEG_SIZE;
end:
	mutex_unlock(&pdata->ops_mutex);
	return ret;
}

static int adv7533_get_raw_edid(void *client,
	u32 size, char *buf, u32 flags)
{
	struct adv7533 *pdata =
		adv7533_get_platform_data(client);

	if (!pdata || !buf) {
		pr_err("%s: invalid data\n", __func__);
		goto end;
	}

	mutex_lock(&pdata->ops_mutex);

	size = min_t(u32, size, sizeof(pdata->edid_buf));

	memcpy(buf, pdata->edid_buf, size);
end:
	mutex_unlock(&pdata->ops_mutex);
	return 0;
}

static int adv7533_write_reg(struct msm_dba_device_info *dev,
		u32 reg, u32 val)
{
	struct adv7533 *pdata;
	int ret = -EINVAL;
	u8 i2c_addr = 0;

	if (!dev)
		goto end;

	pdata = container_of(dev, struct adv7533, dev_info);
	if (!pdata)
		goto end;

	i2c_addr = ((reg & 0x100) ? I2C_ADDR_CEC_DSI : I2C_ADDR_MAIN);

	adv7533_write_byte(i2c_addr, (u8)(reg & 0xFF), (u8)(val & 0xFF));
end:
	return ret;
}

static int adv7533_read_reg(struct msm_dba_device_info *dev,
		u32 reg, u32 *val)
{
	int ret = 0;
	u8 byte_val = 0;
	u8 i2c_addr = 0;
	struct adv7533 *pdata;

	if (!dev)
		goto end;

	pdata = container_of(dev, struct adv7533, dev_info);
	if (!pdata)
		goto end;

	i2c_addr = ((reg & 0x100) ? I2C_ADDR_CEC_DSI : I2C_ADDR_MAIN);

	adv7533_read(i2c_addr, (u8)(reg & 0xFF), &byte_val, 1);

	*val = (u32)byte_val;

end:
	return ret;
}

static int adv7533_register_dba(struct adv7533 *pdata)
{
	struct msm_dba_ops *client_ops;
	struct msm_dba_device_ops *dev_ops;

	if (!pdata)
		return -EINVAL;

	client_ops = &pdata->dev_info.client_ops;
	dev_ops = &pdata->dev_info.dev_ops;

	client_ops->power_on        = adv7533_power_on;
	client_ops->video_on        = adv7533_video_on;
	client_ops->configure_audio = adv7533_configure_audio;
	client_ops->hdcp_enable     = adv7533_hdcp_enable;
	client_ops->hdmi_cec_on     = adv7533_cec_enable;
	client_ops->hdmi_cec_write  = adv7533_hdmi_cec_write;
	client_ops->hdmi_cec_read   = adv7533_hdmi_cec_read;
	client_ops->get_edid_size   = adv7533_get_edid_size;
	client_ops->get_raw_edid    = adv7533_get_raw_edid;
	client_ops->check_hpd	    = adv7533_check_hpd;

	dev_ops->write_reg = adv7533_write_reg;
	dev_ops->read_reg = adv7533_read_reg;

	strlcpy(pdata->dev_info.chip_name, "adv7533",
		sizeof(pdata->dev_info.chip_name));

	pdata->dev_info.instance_id = 0;

	mutex_init(&pdata->dev_info.dev_mutex);

	INIT_LIST_HEAD(&pdata->dev_info.client_list);

	return msm_dba_add_probed_device(&pdata->dev_info);
}

static void adv7533_unregister_dba(struct adv7533 *pdata)
{
	if (!pdata)
		return;

	msm_dba_remove_probed_device(&pdata->dev_info);
}

static int adv7533_probe(struct i2c_client *client_,
	 const struct i2c_device_id *id)
{
	static struct adv7533 *pdata;
	int ret = 0;

	client = client_;

	pdata = devm_kzalloc(&client->dev,
		sizeof(struct adv7533), GFP_KERNEL);
	if (!pdata) {
		pr_err("%s: Failed to allocate memory\n", __func__);
		return -ENOMEM;
	}

	ret = adv7533_parse_dt(&client->dev, pdata);
	if (ret) {
		pr_err("%s: Failed to parse DT\n", __func__);
		goto err_dt_parse;
	}

	pdata->i2c_client = client;



	ret = adv7533_config_clocks(pdata, 1);
	if (ret)
		pr_warn("%s: Failed to config clocks\n", __func__);

	ret = adv7533_read_device_rev();
	if (ret != 0) {
		pr_err("%s: Failed to read revision\n", __func__);
		goto p_err;
	}
    
	mutex_init(&pdata->ops_mutex);

	printk(" calling adv7533_register_dba \n");
	ret = adv7533_register_dba(pdata);
	if (ret) {
		pr_err("%s: Error registering with DBA %d\n",
			__func__, ret);
		goto err_dba_reg;
	}

	ret = pinctrl_select_state(pdata->ts_pinctrl,
		pdata->pinctrl_state_active);
	if (ret < 0)
		pr_err("%s: Failed to select %s pinstate %d\n",
			__func__, PINCTRL_STATE_ACTIVE, ret);

	pdata->adv_output = true;


	if (!(pdata->disable_gpios)) {
		ret = adv7533_gpio_configure(pdata, true);
		if (ret) {
			pr_err("%s: Failed to configure GPIOs\n", __func__);
			goto err_gpio_cfg;
		}

		if (pdata->adv_output) {
			gpio_set_value(pdata->switch_gpio, 0);
		} else {
			gpio_set_value(pdata->switch_gpio, 1);
			goto p_err;
		}
	}
      pdata->irq = gpio_to_irq(pdata->irq_gpio);
	ret = request_threaded_irq(pdata->irq, NULL, adv7533_irq,
		IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "adv7533", pdata);
	if (ret) {
		pr_err("%s: Failed to enable ADV7533 interrupt\n",
			__func__);
		goto p_err;
	}

	dev_set_drvdata(&client->dev, &pdata->dev_info);
	ret = msm_dba_helper_sysfs_init(&client->dev);
	if (ret) {
		pr_err("%s: sysfs init failed\n", __func__);
		goto err_dba_helper;
	}
	pdata->workq = create_workqueue("adv7533_workq");
	if (!pdata->workq) {
		pr_err("%s: workqueue creation failed.\n", __func__);
		ret = -EPERM;
		goto err_workqueue;
	}

	pdata->audio_sdev.name = "hdmi_audio";
	if (switch_dev_register(&pdata->audio_sdev) < 0) {
		pr_err("%s: hdmi_audio switch registration failed\n",
			__func__);
		ret = -ENODEV;
		goto p_err;
	}

	switch (pdata->audio) {
	case ADV7533_AUDIO_ON:
		ret = adv7533_write_regs(pdata, I2S_cfg, ARRAY_SIZE(I2S_cfg));
		if (ret != 0) {
			pr_err("%s: I2S configuration fail = %d!\n",
				__func__, ret);
			goto p_err;
		}
		switch_set_state(&pdata->audio_sdev, 1);
		break;
	case ADV7533_AUDIO_OFF:
	default:
		break;
	}

	INIT_DELAYED_WORK(&pdata->adv7533_intr_work_id, adv7533_intr_work);

	pm_runtime_enable(&client->dev);
	pm_runtime_set_active(&client->dev);

	return 0;


	if (pdata->workq)
		destroy_workqueue(pdata->workq);
err_workqueue:
	msm_dba_helper_sysfs_remove(&client->dev);
err_dba_helper:
	disable_irq(pdata->irq);
	free_irq(pdata->irq, pdata);

p_err:
	adv7533_gpio_configure(pdata, false);
err_gpio_cfg:
	adv7533_unregister_dba(pdata);
err_dba_reg:
err_dt_parse:
	devm_kfree(&client->dev, pdata);
	return ret;
}

static int adv7533_remove(struct i2c_client *client)
{
	int ret = -EINVAL;
	struct msm_dba_device_info *dev;
	struct adv7533 *pdata;

	if (!client)
		goto end;

	dev = dev_get_drvdata(&client->dev);
	if (!dev)
		goto end;

	pdata = container_of(dev, struct adv7533, dev_info);
	if (!pdata)
		goto end;

	pm_runtime_disable(&client->dev);
	switch_dev_unregister(&pdata->audio_sdev);
	disable_irq(pdata->irq);
	free_irq(pdata->irq, pdata);

	ret = adv7533_gpio_configure(pdata, false);

	adv7533_config_clocks(pdata, 0);
	devm_kfree(&client->dev, pdata->power_data.clk_config);

	mutex_destroy(&pdata->ops_mutex);

	devm_kfree(&client->dev, pdata);

end:
	return ret;
}

static struct i2c_driver adv7533_driver = {
	.driver = {
		.name = "adv7533",
		.owner = THIS_MODULE,
	},
	.probe = adv7533_probe,
	.remove = adv7533_remove,
	.id_table = adv7533_id,
};

static int __init adv7533_init(void)
{
	return i2c_add_driver(&adv7533_driver);
}

static void __exit adv7533_exit(void)
{
	i2c_del_driver(&adv7533_driver);
}

module_init(adv7533_init);
module_exit(adv7533_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("adv7533 driver");
