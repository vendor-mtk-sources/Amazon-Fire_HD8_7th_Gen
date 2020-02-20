/*
 * linux/sound/soc/codecs/tlv320aic32x4.c
 *
 * Copyright 2011 Vista Silicon S.L.
 *
 * Author: Javier Martin <javier.martin@vista-silicon.com>
 *
 * Based on sound/soc/codecs/wm8974 and TI driver for kernel 2.6.27.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/i2c.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>

#include <sound/tlv320aic32x4.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "tlv320aic32x4.h"

struct aic32x4_configs {
	u32 reg_offset;
	u8 reg_val;
};

static const struct aic32x4_configs biquad_Play_Biscuit[] = {
	/* Playback Filters */
	/* Force DRC (Dynamic Range Compression) off */
	{AIC32X4_DRCCTRLREG1, 0x0F},
};

struct aic32x4_rate_divs {
	u32 mclk;
	u32 rate;
	u8 p_val;
	u8 pll_j;
	u16 pll_d;
	u16 dosr;
	u8 ndac;
	u8 mdac;
	u8 aosr;
	u8 nadc;
	u8 madc;
	u8 blck_N;
};

struct aic32x4_priv {
	struct regmap *regmap;
	u32 sysclk;
	u32 power_cfg;
	u32 micpga_routing;
	bool swapdacs;
	int rstn_gpio;
	struct clk *mclk;
	int channels;
	u16 unmuted;
	bool ignore_ramp;
	bool mfp_amp_muted;
	u32 rate_divs_index;

	struct regulator *supply_ldo;
	struct regulator *supply_iov;
	struct regulator *supply_dv;
	struct regulator *supply_av;
};

/* 0dB min, 0.5dB steps */
static DECLARE_TLV_DB_SCALE(tlv_step_0_5, 0, 50, 0);
/* -63.5dB min, 0.5dB steps */
static DECLARE_TLV_DB_SCALE(tlv_pcm, -6350, 50, 0);
/* -6dB min, 1dB steps */
static DECLARE_TLV_DB_SCALE(tlv_driver_gain, -600, 100, 0);
/* -12dB min, 0.5dB steps */
static DECLARE_TLV_DB_SCALE(tlv_adc_vol, -1200, 50, 0);

static const char * const drc_enable[] = { "Disabled", "Enabled" };
static const struct soc_enum aic32x4_drc_ctrl_reg1 =
		SOC_ENUM_DOUBLE(AIC32X4_DRCCTRLREG1, 6, 5, 2, drc_enable);

static const char * const control_enable[] = { "Off", "On" };

static const char * const dac_soft_stepping_control[] = {
	"1 step/sample", "1 step/2 sample", "disabled"
};
static const struct soc_enum aic32x4_dac_soft_stepping_ctrl =
	SOC_ENUM_SINGLE(AIC32X4_DACSETUP, 0, 3, dac_soft_stepping_control);
static const struct soc_enum aic32x4_mono_enable_enum =
		SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(control_enable), control_enable);
static const struct soc_enum aic32x4_ramp_enable_enum =
		SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(control_enable), control_enable);
static const struct soc_enum aic32x4_mfp_mute_enable_enum =
		SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(control_enable), control_enable);

static u8 dac_soft_stepping = AIC32X4_DAC_DEFAULT_SOFT_STEPPING;

static struct aic32x4_configs biquad_settings_regs[] = {
	/* Playback Filters */
	{AIC32X4_PAGE44 + 12, 0},
	{AIC32X4_PAGE44 + 13, 0},
	{AIC32X4_PAGE44 + 14, 0},
	{AIC32X4_PAGE44 + 16, 0},
	{AIC32X4_PAGE44 + 17, 0},
	{AIC32X4_PAGE44 + 18, 0},
	{AIC32X4_PAGE44 + 20, 0},
	{AIC32X4_PAGE44 + 21, 0},
	{AIC32X4_PAGE44 + 22, 0},
	{AIC32X4_PAGE44 + 24, 0},
	{AIC32X4_PAGE44 + 25, 0},
	{AIC32X4_PAGE44 + 26, 0},
	{AIC32X4_PAGE44 + 28, 0},
	{AIC32X4_PAGE44 + 29, 0},
	{AIC32X4_PAGE44 + 30, 0},
	{AIC32X4_PAGE44 + 32, 0},
	{AIC32X4_PAGE44 + 33, 0},
	{AIC32X4_PAGE44 + 34, 0},
	{AIC32X4_PAGE44 + 36, 0},
	{AIC32X4_PAGE44 + 37, 0},
	{AIC32X4_PAGE44 + 38, 0},
	{AIC32X4_PAGE44 + 40, 0},
	{AIC32X4_PAGE44 + 41, 0},
	{AIC32X4_PAGE44 + 42, 0},
	{AIC32X4_PAGE44 + 44, 0},
	{AIC32X4_PAGE44 + 45, 0},
	{AIC32X4_PAGE44 + 46, 0},
	{AIC32X4_PAGE44 + 48, 0},
	{AIC32X4_PAGE44 + 49, 0},
	{AIC32X4_PAGE44 + 50, 0},
	{AIC32X4_PAGE44 + 52, 0},
	{AIC32X4_PAGE44 + 53, 0},
	{AIC32X4_PAGE44 + 54, 0},
	{AIC32X4_PAGE44 + 56, 0},
	{AIC32X4_PAGE44 + 57, 0},
	{AIC32X4_PAGE44 + 58, 0},
	{AIC32X4_PAGE44 + 60, 0},
	{AIC32X4_PAGE44 + 61, 0},
	{AIC32X4_PAGE44 + 62, 0},
	{AIC32X4_PAGE44 + 64, 0},
	{AIC32X4_PAGE44 + 65, 0},
	{AIC32X4_PAGE44 + 66, 0},
	{AIC32X4_PAGE44 + 68, 0},
	{AIC32X4_PAGE44 + 69, 0},
	{AIC32X4_PAGE44 + 70, 0},
	{AIC32X4_PAGE45 + 20, 0},
	{AIC32X4_PAGE45 + 21, 0},
	{AIC32X4_PAGE45 + 22, 0},
	{AIC32X4_PAGE45 + 24, 0},
	{AIC32X4_PAGE45 + 25, 0},
	{AIC32X4_PAGE45 + 26, 0},
	{AIC32X4_PAGE45 + 28, 0},
	{AIC32X4_PAGE45 + 29, 0},
	{AIC32X4_PAGE45 + 30, 0},
	{AIC32X4_PAGE45 + 32, 0},
	{AIC32X4_PAGE45 + 33, 0},
	{AIC32X4_PAGE45 + 34, 0},
	{AIC32X4_PAGE45 + 36, 0},
	{AIC32X4_PAGE45 + 37, 0},
	{AIC32X4_PAGE45 + 38, 0},
	{AIC32X4_PAGE45 + 40, 0},
	{AIC32X4_PAGE45 + 41, 0},
	{AIC32X4_PAGE45 + 42, 0},
	{AIC32X4_PAGE45 + 44, 0},
	{AIC32X4_PAGE45 + 45, 0},
	{AIC32X4_PAGE45 + 46, 0},
	{AIC32X4_PAGE45 + 48, 0},
	{AIC32X4_PAGE45 + 49, 0},
	{AIC32X4_PAGE45 + 50, 0},
	{AIC32X4_PAGE45 + 52, 0},
	{AIC32X4_PAGE45 + 53, 0},
	{AIC32X4_PAGE45 + 54, 0},
	{AIC32X4_PAGE45 + 56, 0},
	{AIC32X4_PAGE45 + 57, 0},
	{AIC32X4_PAGE45 + 58, 0},
	{AIC32X4_PAGE45 + 60, 0},
	{AIC32X4_PAGE45 + 61, 0},
	{AIC32X4_PAGE45 + 62, 0},
	{AIC32X4_PAGE45 + 64, 0},
	{AIC32X4_PAGE45 + 65, 0},
	{AIC32X4_PAGE45 + 66, 0},
	{AIC32X4_PAGE45 + 68, 0},
	{AIC32X4_PAGE45 + 69, 0},
	{AIC32X4_PAGE45 + 70, 0},
	{AIC32X4_PAGE45 + 72, 0},
	{AIC32X4_PAGE45 + 73, 0},
	{AIC32X4_PAGE45 + 74, 0},
	{AIC32X4_PAGE45 + 76, 0},
	{AIC32X4_PAGE45 + 77, 0},
	{AIC32X4_PAGE45 + 78, 0},
	{AIC32X4_PAGE46 + 52, 0},    /* DRC HPF: Page 46: N0 52 - 55 */
	{AIC32X4_PAGE46 + 53, 0},    /* Bypassed: N0 = 0x7FFFFF */
	{AIC32X4_PAGE46 + 54, 0},
	{AIC32X4_PAGE46 + 55, 0},    /* N1 = 0 */
	{AIC32X4_PAGE46 + 56, 0},
	{AIC32X4_PAGE46 + 57, 0},
	{AIC32X4_PAGE46 + 58, 0},
	{AIC32X4_PAGE46 + 59, 0},
	{AIC32X4_PAGE46 + 60, 0},   /* D1 */
	{AIC32X4_PAGE46 + 61, 0},
	{AIC32X4_PAGE46 + 62, 0},
	{AIC32X4_PAGE46 + 63, 0},
	{AIC32X4_PAGE46 + 64, 0},    /* DRC LPF: Page 46: N0 64 - 67 */
	{AIC32X4_PAGE46 + 65, 0},    /* Bypassed: N0 = 0x7FFFFF */
	{AIC32X4_PAGE46 + 66, 0},
	{AIC32X4_PAGE46 + 67, 0},    /* N1 = 0 */
	{AIC32X4_PAGE46 + 68, 0},
	{AIC32X4_PAGE46 + 69, 0},
	{AIC32X4_PAGE46 + 70, 0},
	{AIC32X4_PAGE46 + 71, 0},
	{AIC32X4_PAGE46 + 72, 0},   /* D1 */
	{AIC32X4_PAGE46 + 73, 0},
	{AIC32X4_PAGE46 + 74, 0},
	{AIC32X4_PAGE46 + 75, 0},
#ifdef SET_DRC_OFF
	/* Force DRC (Dynamic Range Compression) off */
	{AIC32X4_DRCCTRLREG1, 0x0},
#else
	/* Enable DRC */
	{AIC32X4_DRCCTRLREG1, 0x0},
	{AIC32X4_DRCCTRLREG2, 0x0},
	{AIC32X4_DRCCTRLREG3, 0x0}
#endif
};

#define BIQUAD_SETTINGS_REGS_SIZE \
	(sizeof(biquad_settings_regs)/sizeof(struct aic32x4_configs))

static bool aic32x4_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AIC32X4_PSEL: /* regmap implementation requires this */
	case AIC32X4_RESET: /* always clears after write */
	case AIC32X4_GAIN_APPLIED:
		return true;
	}
	return false;
}

static int aic32x4_biquad_coeff_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	int i;

	if (bytes_ext->max != BIQUAD_SETTINGS_REGS_SIZE)
		return -EINVAL;

	for (i = 0; i < BIQUAD_SETTINGS_REGS_SIZE; i++) {
		ucontrol->value.bytes.data[i] =
			biquad_settings_regs[i].reg_val;
	}

	return 0;
}

static int aic32x4_biquad_coeff_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct soc_bytes_ext *bytes_ext =
			(struct soc_bytes_ext *) kcontrol->private_value;
	int i;

	if (bytes_ext->max != BIQUAD_SETTINGS_REGS_SIZE)
		return -EINVAL;

	for (i = 0; i < BIQUAD_SETTINGS_REGS_SIZE; i++) {
		biquad_settings_regs[i].reg_val = ucontrol->value.bytes.data[i];
		snd_soc_write(codec,
			biquad_settings_regs[i].reg_offset,
						ucontrol->value.bytes.data[i]);
	}

	return 0;
}

static int snd_soc_put_enum_double_wrapper(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int *item = ucontrol->value.enumerated.item;

	if (item[0] >= e->items)
		return -EINVAL;

	dac_soft_stepping =
		snd_soc_enum_item_to_val(e, item[0]) << e->shift_l;

	return snd_soc_put_enum_double(kcontrol, ucontrol);
}

static int aic32x4_apply_mute(struct snd_soc_codec *codec, int mute)
{
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);
	u8 unmute_state, prog_gain, ref_gain;
	int i = 0;

	dev_dbg(codec->dev, "%s:+ mute=%d\n", __func__, mute);

	unmute_state = snd_soc_read(codec, AIC32X4_DACMUTE) & ~AIC32X4_MUTEON;
	prog_gain = snd_soc_read(codec, AIC32X4_GAIN_APPLIED);

	if (mute) {
		if (aic32x4->mfp_amp_muted == false) {
			/* DEE-43236: Device reboots if Amp is unmute */
			/* Need to Mute AMP connected to MFP2 first */
			snd_soc_write(codec, AIC32X4_DOUTCTL,
				MFP2_GPIO_ENABLE|MFP2_GPIO_HI);
			aic32x4->mfp_amp_muted = true;
		}
		snd_soc_write(codec, AIC32X4_DACMUTE,
			unmute_state | AIC32X4_MUTEON);
	} else {
		if (aic32x4->channels == 1) {
			/* For mono, check HP Driver only for one channel
			 * and unmute one channel too
			 */
			unmute_state |= AIC32X4_UNMUTE_MONO;
			ref_gain = DAC_GAIN_MONO_APPLIED;
		} else {
			ref_gain = DAC_GAIN_STEREO_APPLIED;
		}

		if (aic32x4->ignore_ramp == false) {
			/* Before unmuting wait for DAC gain to reach applied
			 * level. Not doing so will cause a pop. It can take
			 * upto 2sec. Limit the wait by a counter.
			 */
			while (prog_gain < ref_gain && i < 22) {
				/* wait for applied gain to reduce Pop */
				prog_gain = snd_soc_read(codec,
						AIC32X4_GAIN_APPLIED);
				mdelay(100);
				i++;
			}
		}

		/* Unnmute required number of channels */
		snd_soc_write(codec, AIC32X4_DACMUTE, unmute_state);
	}
	/* Store device current status */
	aic32x4->unmuted = !mute;

	dev_info(codec->dev, "%s: Playback_Audio amp_mute=%d codec_unmute=%d prog_gain=%x i=%d\n",
		__func__, aic32x4->mfp_amp_muted, aic32x4->unmuted, prog_gain, i);

	return 0;
}

static int put_right_ch_enab_only(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);
	int prev_channels = aic32x4->channels;

	dev_info(codec->dev, "%s right_only=%d, current_ch=%d unmute=%d\n",
		__func__, ucontrol->value.enumerated.item[0], aic32x4->channels,
		aic32x4->unmuted);

	if (ucontrol->value.enumerated.item[0] >
		ARRAY_SIZE(control_enable)) {
		pr_err("%s: Mono Channel Settings Invalid value=%d\n", __func__,
			ucontrol->value.enumerated.item[0]);
		return -EINVAL;
	}

	if (ucontrol->value.enumerated.item[0])
		aic32x4->channels = 1;
	else
		aic32x4->channels = 2;

	/* If channel count was updated and DAC is in unmute state,
	 * apply unmute to correct number of channels
	 */
	if (prev_channels != aic32x4->channels && aic32x4->unmuted)
		aic32x4_apply_mute(codec, 0);

	return 0;
}

static int get_right_ch_enab_only(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s channels=%d unmute=%d\n", __func__,
		aic32x4->channels, aic32x4->unmuted);

	if (aic32x4->channels == 1)
		ucontrol->value.enumerated.item[0] = 1;
	else
		ucontrol->value.enumerated.item[0] = 0;

	return 0;
}

static int set_ignore_ramp(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);

	dev_info(codec->dev, "%s ignore_set=%d previous_value=%d\n", __func__,
		ucontrol->value.enumerated.item[0], aic32x4->ignore_ramp);

	if (ucontrol->value.enumerated.item[0] >
		ARRAY_SIZE(control_enable)) {
		pr_err("%s: Ignore Ramp value Invalid. Value=%d\n", __func__,
			ucontrol->value.enumerated.item[0]);
		return -EINVAL;
	}

	if (ucontrol->value.enumerated.item[0]) {
		/* Headphone Driver Startup Control */
		snd_soc_write(codec, AIC32X4_HEADSTART,
			HP_AMP_STARTUP_DELAY_DISABLED);
		aic32x4->ignore_ramp = true;
	} else {
		snd_soc_write(codec, AIC32X4_HEADSTART,
			HP_AMP_SOFT_ROUTE_STARTUP_DELAY);
		aic32x4->ignore_ramp = false;
	}

	return 0;
}

static int get_ignore_ramp(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s ignore_ramp=%d\n", __func__,
		aic32x4->ignore_ramp);

	ucontrol->value.enumerated.item[0] = aic32x4->ignore_ramp;

	return 0;
}

static int set_mfp_mute_enable(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);

	if (ucontrol->value.enumerated.item[0] >
		ARRAY_SIZE(control_enable)) {
		pr_err("%s: MFP Gpio Mute. Invalid value=%d\n", __func__,
			ucontrol->value.enumerated.item[0]);
		return -EINVAL;
	}

	if (ucontrol->value.enumerated.item[0]) {
		/* Mute AMP connected to MFP2 */
		snd_soc_write(codec, AIC32X4_DOUTCTL,
			MFP2_GPIO_ENABLE|MFP2_GPIO_HI);
		aic32x4->mfp_amp_muted = true;
	} else {
		/* Unmute AMP connected to MFP2 */
		snd_soc_write(codec, AIC32X4_DOUTCTL, MFP2_GPIO_ENABLE);
		aic32x4->mfp_amp_muted = false;
	}

	dev_info(codec->dev, "%s: Playback_Audio amp_mute=%d codec_unmute=%d\n",
		__func__, aic32x4->mfp_amp_muted, aic32x4->unmuted);

	return 0;
}

static int get_mfp_mute_enable(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s amp_mute=%d channels=%d unmute=%d\n", __func__,
		aic32x4->mfp_amp_muted, aic32x4->channels, aic32x4->unmuted);

	if (aic32x4->mfp_amp_muted)
		ucontrol->value.enumerated.item[0] = 1;
	else
		ucontrol->value.enumerated.item[0] = 0;

	return 0;
}


static const struct snd_kcontrol_new aic32x4_snd_controls[] = {
	SOC_DOUBLE_R_S_TLV("PCM Playback Volume", AIC32X4_LDACVOL,
			AIC32X4_RDACVOL, 0, -0x7f, 0x30, 7, 0, tlv_pcm),
	SOC_DOUBLE_R_S_TLV("HP Driver Gain Volume", AIC32X4_HPLGAIN,
			AIC32X4_HPRGAIN, 0, -0x6, 0x1d, 5, 0,
			tlv_driver_gain),
	SOC_DOUBLE_R_S_TLV("LO Driver Gain Volume", AIC32X4_LOLGAIN,
			AIC32X4_LORGAIN, 0, -0x6, 0x1d, 5, 0,
			tlv_driver_gain),
	SOC_DOUBLE_R("HP DAC Playback Switch", AIC32X4_HPLGAIN,
			AIC32X4_HPRGAIN, 6, 0x01, 1),
	SOC_DOUBLE_R("LO DAC Playback Switch", AIC32X4_LOLGAIN,
			AIC32X4_LORGAIN, 6, 0x01, 1),
	SOC_DOUBLE_R("Mic PGA Switch", AIC32X4_LMICPGAVOL,
			AIC32X4_RMICPGAVOL, 7, 0x01, 1),

	SOC_ENUM("DRC Control", aic32x4_drc_ctrl_reg1),

	SOC_SINGLE("ADCFGA Left Mute Switch", AIC32X4_ADCFGA, 7, 1, 0),
	SOC_SINGLE("ADCFGA Right Mute Switch", AIC32X4_ADCFGA, 3, 1, 0),

	SOC_DOUBLE_R_S_TLV("ADC Level Volume", AIC32X4_LADCVOL,
			AIC32X4_RADCVOL, 0, -0x18, 0x28, 6, 0, tlv_adc_vol),
	SOC_DOUBLE_R_TLV("PGA Level Volume", AIC32X4_LMICPGAVOL,
			AIC32X4_RMICPGAVOL, 0, 0x5f, 0, tlv_step_0_5),

	SOC_SINGLE("Auto-mute Switch", AIC32X4_DACMUTE, 4, 7, 0),

	SOC_SINGLE("AGC Left Switch", AIC32X4_LAGC1, 7, 1, 0),
	SOC_SINGLE("AGC Right Switch", AIC32X4_RAGC1, 7, 1, 0),
	SOC_DOUBLE_R("AGC Target Level", AIC32X4_LAGC1, AIC32X4_RAGC1,
			4, 0x07, 0),
	SOC_DOUBLE_R("AGC Gain Hysteresis", AIC32X4_LAGC1, AIC32X4_RAGC1,
			0, 0x03, 0),
	SOC_DOUBLE_R("AGC Hysteresis", AIC32X4_LAGC2, AIC32X4_RAGC2,
			6, 0x03, 0),
	SOC_DOUBLE_R("AGC Noise Threshold", AIC32X4_LAGC2, AIC32X4_RAGC2,
			1, 0x1F, 0),
	SOC_DOUBLE_R("AGC Max PGA", AIC32X4_LAGC3, AIC32X4_RAGC3,
			0, 0x7F, 0),
	SOC_DOUBLE_R("AGC Attack Time", AIC32X4_LAGC4, AIC32X4_RAGC4,
			3, 0x1F, 0),
	SOC_DOUBLE_R("AGC Decay Time", AIC32X4_LAGC5, AIC32X4_RAGC5,
			3, 0x1F, 0),
	SOC_DOUBLE_R("AGC Noise Debounce", AIC32X4_LAGC6, AIC32X4_RAGC6,
			0, 0x1F, 0),
	SOC_DOUBLE_R("AGC Signal Debounce", AIC32X4_LAGC7, AIC32X4_RAGC7,
			0, 0x0F, 0),
	SND_SOC_BYTES_EXT("biquad coefficients",
				BIQUAD_SETTINGS_REGS_SIZE,
				aic32x4_biquad_coeff_get,
				aic32x4_biquad_coeff_set),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "DAC Soft Stepping",
		.info = snd_soc_info_enum_double,
		.get = snd_soc_get_enum_double,
		.put = snd_soc_put_enum_double_wrapper,
		.private_value = (unsigned long)&aic32x4_dac_soft_stepping_ctrl
	},
	SOC_ENUM_EXT("Right Channel Only", aic32x4_mono_enable_enum,
		get_right_ch_enab_only, put_right_ch_enab_only),
	SOC_ENUM_EXT("Ignore Ramp Up", aic32x4_ramp_enable_enum,
		get_ignore_ramp, set_ignore_ramp),
	SOC_ENUM_EXT("MFP Gpio Mute", aic32x4_mfp_mute_enable_enum,
		get_mfp_mute_enable, set_mfp_mute_enable)
};

static const struct aic32x4_rate_divs aic32x4_divs[] = {
	/* 8k rate */
	{AIC32X4_FREQ_12000000, 8000, 1, 7, 6800, 768, 5, 3, 128, 5, 18, 24},
	{AIC32X4_FREQ_24000000, 8000, 2, 7, 6800, 768, 15, 1, 64, 45, 4, 24},
	{AIC32X4_FREQ_25000000, 8000, 2, 7, 3728, 768, 15, 1, 64, 45, 4, 24},
	/* 11.025k rate */
	{AIC32X4_FREQ_12000000, 11025, 1, 7, 5264, 512, 8, 2, 128, 8, 8, 16},
	{AIC32X4_FREQ_24000000, 11025, 2, 7, 5264, 512, 16, 1, 64, 32, 4, 16},
	/* 16k rate */
	{AIC32X4_FREQ_12000000, 16000, 1, 7, 6800, 384, 5, 3, 128, 5, 9, 12},
	{AIC32X4_FREQ_24000000, 16000, 2, 7, 6800, 384, 15, 1, 64, 18, 5, 12},
	{AIC32X4_FREQ_25000000, 16000, 2, 7, 3728, 384, 15, 1, 64, 18, 5, 12},
	/* 22.05k rate */
	{AIC32X4_FREQ_12000000, 22050, 1, 7, 5264, 256, 4, 4, 128, 4, 8, 8},
	{AIC32X4_FREQ_24000000, 22050, 2, 7, 5264, 256, 16, 1, 64, 16, 4, 8},
	{AIC32X4_FREQ_25000000, 22050, 2, 7, 2253, 256, 16, 1, 64, 16, 4, 8},
	/* 32k rate */
	{AIC32X4_FREQ_12000000, 32000, 1, 7, 1680, 192, 2, 7, 64, 2, 21, 6},
	{AIC32X4_FREQ_24000000, 32000, 2, 7, 1680, 192, 7, 2, 64, 7, 6, 6},
	/* 44.1k rate */
	{AIC32X4_FREQ_12000000, 44100, 1, 7, 5264, 128, 2, 8, 128, 2, 8, 4},
	{AIC32X4_FREQ_24000000, 44100, 2, 7, 5264, 128, 8, 2, 64, 8, 4, 4},
	{AIC32X4_FREQ_25000000, 44100, 2, 7, 2253, 128, 8, 2, 64, 8, 4, 4},
	/* 48k rate */
	{AIC32X4_FREQ_9600000,  48000, 5, 48, 0,   128, 5, 3, 128, 5, 3, 1},
	{AIC32X4_FREQ_12000000, 48000, 1, 8, 1920, 128, 2, 8, 128, 2, 8, 4},
	{AIC32X4_FREQ_24000000, 48000, 2, 8, 1920, 128, 8, 2, 64, 8, 4, 4},
	{AIC32X4_FREQ_24576000, 48000, 0, 0,    0, 128, 1, 4, 128, 1, 4, 0},
	{AIC32X4_FREQ_25000000, 48000, 2, 7, 8643, 128, 8, 2, 64, 8, 4, 4},
};

static const struct snd_kcontrol_new hpl_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("L_DAC Switch", AIC32X4_HPLROUTE, 3, 1, 0),
	SOC_DAPM_SINGLE("IN1_L Switch", AIC32X4_HPLROUTE, 2, 1, 0),
};

static const struct snd_kcontrol_new hpr_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("R_DAC Switch", AIC32X4_HPRROUTE, 3, 1, 0),
	SOC_DAPM_SINGLE("IN1_R Switch", AIC32X4_HPRROUTE, 2, 1, 0),
};

static const struct snd_kcontrol_new lol_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("L_DAC Switch", AIC32X4_LOLROUTE, 3, 1, 0),
};

static const struct snd_kcontrol_new lor_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("R_DAC Switch", AIC32X4_LORROUTE, 3, 1, 0),
};

static const struct snd_kcontrol_new left_input_mixer_controls[] = {
	SOC_DAPM_SINGLE("IN1_L P Switch", AIC32X4_LMICPGAPIN, 6, 1, 0),
	SOC_DAPM_SINGLE("IN2_L P Switch", AIC32X4_LMICPGAPIN, 4, 1, 0),
	SOC_DAPM_SINGLE("IN3_L P Switch", AIC32X4_LMICPGAPIN, 2, 1, 0),
};

static const struct snd_kcontrol_new right_input_mixer_controls[] = {
	SOC_DAPM_SINGLE("IN1_R P Switch", AIC32X4_RMICPGAPIN, 6, 1, 0),
	SOC_DAPM_SINGLE("IN2_R P Switch", AIC32X4_RMICPGAPIN, 4, 1, 0),
	SOC_DAPM_SINGLE("IN3_R P Switch", AIC32X4_RMICPGAPIN, 2, 1, 0),
};

static const struct snd_soc_dapm_widget aic32x4_dapm_widgets[] = {
	SND_SOC_DAPM_DAC("Left DAC", "Left Playback", AIC32X4_DACSETUP, 7, 0),
	SND_SOC_DAPM_MIXER("HPL Output Mixer", SND_SOC_NOPM, 0, 0,
			   &hpl_output_mixer_controls[0],
			   ARRAY_SIZE(hpl_output_mixer_controls)),
	SND_SOC_DAPM_PGA("HPL Power", AIC32X4_OUTPWRCTL, 5, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("LOL Output Mixer", SND_SOC_NOPM, 0, 0,
			   &lol_output_mixer_controls[0],
			   ARRAY_SIZE(lol_output_mixer_controls)),
	SND_SOC_DAPM_PGA("LOL Power", AIC32X4_OUTPWRCTL, 3, 0, NULL, 0),

	SND_SOC_DAPM_DAC("Right DAC", "Right Playback", AIC32X4_DACSETUP, 6, 0),
	SND_SOC_DAPM_MIXER("HPR Output Mixer", SND_SOC_NOPM, 0, 0,
			   &hpr_output_mixer_controls[0],
			   ARRAY_SIZE(hpr_output_mixer_controls)),
	SND_SOC_DAPM_PGA("HPR Power", AIC32X4_OUTPWRCTL, 4, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("LOR Output Mixer", SND_SOC_NOPM, 0, 0,
			   &lor_output_mixer_controls[0],
			   ARRAY_SIZE(lor_output_mixer_controls)),
	SND_SOC_DAPM_PGA("LOR Power", AIC32X4_OUTPWRCTL, 2, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("Left Input Mixer", SND_SOC_NOPM, 0, 0,
			   &left_input_mixer_controls[0],
			   ARRAY_SIZE(left_input_mixer_controls)),
	SND_SOC_DAPM_MIXER("Right Input Mixer", SND_SOC_NOPM, 0, 0,
			   &right_input_mixer_controls[0],
			   ARRAY_SIZE(right_input_mixer_controls)),
	SND_SOC_DAPM_ADC("Left ADC", "Left Capture", AIC32X4_ADCSETUP, 7, 0),
	SND_SOC_DAPM_ADC("Right ADC", "Right Capture", AIC32X4_ADCSETUP, 6, 0),
	SND_SOC_DAPM_MICBIAS("Mic Bias", AIC32X4_MICBIAS, 6, 0),

	SND_SOC_DAPM_OUTPUT("HPL"),
	SND_SOC_DAPM_OUTPUT("HPR"),
	SND_SOC_DAPM_OUTPUT("LOL"),
	SND_SOC_DAPM_OUTPUT("LOR"),
	SND_SOC_DAPM_INPUT("IN1_L"),
	SND_SOC_DAPM_INPUT("IN1_R"),
	SND_SOC_DAPM_INPUT("IN2_L"),
	SND_SOC_DAPM_INPUT("IN2_R"),
	SND_SOC_DAPM_INPUT("IN3_L"),
	SND_SOC_DAPM_INPUT("IN3_R"),
};

static const struct snd_soc_dapm_route aic32x4_dapm_routes[] = {
	/* Left Output */
	{"HPL Output Mixer", "L_DAC Switch", "Left DAC"},
	{"HPL Output Mixer", "IN1_L Switch", "IN1_L"},

	{"HPL Power", NULL, "HPL Output Mixer"},
	{"HPL", NULL, "HPL Power"},

	{"LOL Output Mixer", "L_DAC Switch", "Left DAC"},

	{"LOL Power", NULL, "LOL Output Mixer"},
	{"LOL", NULL, "LOL Power"},

	/* Right Output */
	{"HPR Output Mixer", "R_DAC Switch", "Right DAC"},
	{"HPR Output Mixer", "IN1_R Switch", "IN1_R"},

	{"HPR Power", NULL, "HPR Output Mixer"},
	{"HPR", NULL, "HPR Power"},

	{"LOR Output Mixer", "R_DAC Switch", "Right DAC"},

	{"LOR Power", NULL, "LOR Output Mixer"},
	{"LOR", NULL, "LOR Power"},

	/* Left input */
	{"Left Input Mixer", "IN1_L P Switch", "IN1_L"},
	{"Left Input Mixer", "IN2_L P Switch", "IN2_L"},
	{"Left Input Mixer", "IN3_L P Switch", "IN3_L"},

	{"Left ADC", NULL, "Left Input Mixer"},

	/* Right Input */
	{"Right Input Mixer", "IN1_R P Switch", "IN1_R"},
	{"Right Input Mixer", "IN2_R P Switch", "IN2_R"},
	{"Right Input Mixer", "IN3_R P Switch", "IN3_R"},

	{"Right ADC", NULL, "Right Input Mixer"},
};

static const struct regmap_range_cfg aic32x4_regmap_pages[] = {
	{
		.selector_reg = 0,
		.selector_mask  = 0xff,
		.window_start = 0,
		.window_len = 128,
		.range_min = 0,
		.range_max = AIC32X4_REG_MAX_RANGE,
	},
};

static const struct regmap_config aic32x4_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_reg = aic32x4_volatile,
	.max_register = AIC32X4_REG_MAX_RANGE,
	.ranges = aic32x4_regmap_pages,
	.num_ranges = ARRAY_SIZE(aic32x4_regmap_pages),
};

static inline int aic32x4_get_divs(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(aic32x4_divs); i++) {
		if ((aic32x4_divs[i].rate == rate)
		    && (aic32x4_divs[i].mclk == mclk)) {
			return i;
		}
	}
	pr_err("aic32x4:%s master clock %d and sample rate %d is not supported\n",
		__func__, mclk, rate);
	return -EINVAL;
}

static int aic32x4_set_dai_sysclk(struct snd_soc_dai *codec_dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);

	switch (freq) {
	case AIC32X4_FREQ_9600000:
	case AIC32X4_FREQ_12000000:
	case AIC32X4_FREQ_24000000:
	case AIC32X4_FREQ_24576000:
	case AIC32X4_FREQ_25000000:
		aic32x4->sysclk = freq;
		return 0;
	}
	pr_err("aic32x4:%s invalid frequency %u to set DAI system clock\n",
		__func__, freq);
	return -EINVAL;
}

static int aic32x4_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u8 iface_reg_1;
	u8 iface_reg_2;
	u8 iface_reg_3;

	dev_dbg(codec->dev, "%s: dai fmt\n", __func__);

	iface_reg_1 = snd_soc_read(codec, AIC32X4_IFACE1);
	iface_reg_1 = iface_reg_1 & ~(3 << 6 | 3 << 2);
	iface_reg_2 = 0;
	iface_reg_3 = snd_soc_read(codec, AIC32X4_IFACE3);
	iface_reg_3 = iface_reg_3 & ~(1 << 3);

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		iface_reg_1 |= AIC32X4_BCLKMASTER | AIC32X4_WCLKMASTER;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
			iface_reg_3 |= AIC32X4_DACMOD2BCLK;
		break;
	default:
		pr_err("aic32x4:%s: invalid DAI master/slave interface\n",
			__func__);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_DSP_A:
		iface_reg_1 |= (AIC32X4_DSP_MODE << AIC32X4_PLLJ_SHIFT);
		iface_reg_3 |= (1 << 3); /* invert bit clock */
		iface_reg_2 = 0x01; /* add offset 1 */
		break;
	case SND_SOC_DAIFMT_DSP_B:
		iface_reg_1 |= (AIC32X4_DSP_MODE << AIC32X4_PLLJ_SHIFT);
		iface_reg_3 |= (1 << 3); /* invert bit clock */
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		iface_reg_1 |=
			(AIC32X4_RIGHT_JUSTIFIED_MODE << AIC32X4_PLLJ_SHIFT);
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface_reg_1 |=
			(AIC32X4_LEFT_JUSTIFIED_MODE << AIC32X4_PLLJ_SHIFT);
		break;
	default:
		pr_err("aic32x4:%s invalid DAI interface format\n", __func__);
		return -EINVAL;
	}

	snd_soc_write(codec, AIC32X4_IFACE1, iface_reg_1);
	snd_soc_write(codec, AIC32X4_IFACE2, iface_reg_2);
	snd_soc_write(codec, AIC32X4_IFACE3, iface_reg_3);
	return 0;
}

static int aic32x4_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);
	u8 data;
	int i, j, size, width, stream_channels;
	const struct aic32x4_configs *pConfigRegs;

	width = params_width(params);
	stream_channels = params_channels(params);
	dev_info(codec->dev, "%s+ stream_ch=%d dac_ch=%d width=%d ignore_ramping=%d\n",
		__func__, stream_channels, aic32x4->channels, width,
		aic32x4->ignore_ramp);

	i = aic32x4_get_divs(aic32x4->sysclk, params_rate(params));
	if (i < 0) {
		dev_err(codec->dev, "aic32x4: sampling rate not supported\n");
		return i;
	}
	aic32x4->rate_divs_index = i;

	/* Mute HPL Driver to avoid pop  */
	snd_soc_update_bits(codec, AIC32X4_HPLGAIN, HP_DRIVER_MUTE_MUX,
		HP_DRIVER_MUTE);
	/* Mute HPR Driver to avoid pop  */
	snd_soc_update_bits(codec, AIC32X4_HPRGAIN, HP_DRIVER_MUTE_MUX,
		HP_DRIVER_MUTE);

	if (aic32x4_divs[i].p_val) {
		/* Use PLL as CODEC_CLKIN */
		snd_soc_write(codec, AIC32X4_CLKMUX, AIC32X4_PLLCLKIN);

		/* We will fix R = 1 and will make P & J=K.D as varialble */
		snd_soc_write(codec, AIC32X4_PLLPR,
			((aic32x4_divs[i].p_val << 4) | 0x01 | AIC32X4_PLLEN));

		snd_soc_write(codec, AIC32X4_PLLJ, aic32x4_divs[i].pll_j);

		snd_soc_write(codec, AIC32X4_PLLDMSB,
			(aic32x4_divs[i].pll_d >> 8));
		snd_soc_write(codec, AIC32X4_PLLDLSB,
			(aic32x4_divs[i].pll_d & 0xff));
	}

	/* NDAC divider value */
	snd_soc_write(codec, AIC32X4_NDAC,
		aic32x4_divs[i].ndac | AIC32X4_NDACEN);

	/* MDAC divider value */
	snd_soc_write(codec, AIC32X4_MDAC,
		aic32x4_divs[i].mdac | AIC32X4_MDACEN);

	/* DOSR MSB & LSB values */
	snd_soc_write(codec, AIC32X4_DOSRMSB, aic32x4_divs[i].dosr >> 8);
	snd_soc_write(codec, AIC32X4_DOSRLSB, (aic32x4_divs[i].dosr & 0xff));

	/* NADC divider value */
	data = snd_soc_read(codec, AIC32X4_NADC);
	data &= ~(0x7f);
	snd_soc_write(codec, AIC32X4_NADC, data | aic32x4_divs[i].nadc);

	/* MADC divider value */
	data = snd_soc_read(codec, AIC32X4_MADC);
	data &= ~(0x7f);
	snd_soc_write(codec, AIC32X4_MADC, data | aic32x4_divs[i].madc);

	/* AOSR value */
	snd_soc_write(codec, AIC32X4_AOSR, aic32x4_divs[i].aosr);

	if (aic32x4_divs[i].blck_N) {
		/* Set and Enable BCLK N divider */
		snd_soc_write(codec, AIC32X4_BCLKN,
			aic32x4_divs[i].blck_N | AIC32X4_BCLKEN);
	}

	data = snd_soc_read(codec, AIC32X4_IFACE1);
	data = data & ~(3 << 4);
	switch (width) {
	case 16:
		break;
	case 20:
		data |= (AIC32X4_WORD_LEN_20BITS << AIC32X4_DOSRMSB_SHIFT);
		break;
	case 24:
		data |= (AIC32X4_WORD_LEN_24BITS << AIC32X4_DOSRMSB_SHIFT);
		break;
	case 32:
		data |= (AIC32X4_WORD_LEN_32BITS << AIC32X4_DOSRMSB_SHIFT);
		break;
	}
	snd_soc_write(codec, AIC32X4_IFACE1, data);

	if (stream_channels == 1) {
		data = AIC32X4_RDAC2RCHN | AIC32X4_LDAC2RCHN;
	} else {
		if (aic32x4->swapdacs)
			data = AIC32X4_RDAC2LCHN | AIC32X4_LDAC2RCHN;
		else
			data = AIC32X4_LDAC2LCHN | AIC32X4_RDAC2RCHN;
	}

	/* Set the signal processing block (PRB) modes */
	snd_soc_write(codec, AIC32X4_DACSPB, 0x2);

	/* Program the biquads and DRC */
	pConfigRegs = biquad_Play_Biscuit;
	size = ARRAY_SIZE(biquad_Play_Biscuit);
	for (j = 0; j < size; j++) {
		/* Get the register offset and value */
		snd_soc_write(codec,
			pConfigRegs[j].reg_offset, pConfigRegs[j].reg_val);
	}

	/* Headphone Driver Startup Control if output ramping is enabled */
	if (aic32x4->ignore_ramp)
		snd_soc_write(codec, AIC32X4_HEADSTART,
			HP_AMP_STARTUP_DELAY_DISABLED);
	else
		snd_soc_write(codec, AIC32X4_HEADSTART,
			HP_AMP_SOFT_ROUTE_STARTUP_DELAY);

	/* Enable soft stepping on volume change (ramp) */
	data |= dac_soft_stepping;
	snd_soc_update_bits(codec, AIC32X4_DACSETUP,
			(AIC32X4_DAC_CHAN_MASK |
			AIC32X4_DAC_SOFT_STEPPING_MASK), data);

	/* For stereo enable both. Otherwise only enable right channel */
	if (stream_channels == 2) {
		/* Unmute HPL Driver to avoid pop */
		snd_soc_update_bits(codec, AIC32X4_HPLGAIN, HP_DRIVER_MUTE_MUX,
			HP_DRIVER_UNMUTE);
	}
	/* Unmute HPR Driver to avoid pop */
	snd_soc_update_bits(codec, AIC32X4_HPRGAIN, HP_DRIVER_MUTE_MUX,
		HP_DRIVER_UNMUTE);

	dev_dbg(codec->dev, "%s-\n", __func__);

	return 0;
}

static int aic32x4_mute(struct snd_soc_dai *dai, int mute)
{
	return aic32x4_apply_mute(dai->codec, mute);
}

static int aic32x4_set_bias_level(struct snd_soc_codec *codec,
				  enum snd_soc_bias_level level)
{
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	dev_info(codec->dev, "%s bias=%d\n", __func__, level);

	switch (level) {
	case SND_SOC_BIAS_ON:
		/* Switch on master clock */
		ret = clk_prepare_enable(aic32x4->mclk);
		if (ret)
			dev_err(codec->dev, "Failed to enable master clock\n");

		/* Switch on PLL */
		if (aic32x4_divs[aic32x4->rate_divs_index].p_val)
			snd_soc_update_bits(codec, AIC32X4_PLLPR,
				AIC32X4_PLLEN, AIC32X4_PLLEN);

		/* Switch on NDAC Divider */
		snd_soc_update_bits(codec, AIC32X4_NDAC,
				    AIC32X4_NDACEN, AIC32X4_NDACEN);

		/* Switch on MDAC Divider */
		snd_soc_update_bits(codec, AIC32X4_MDAC,
				    AIC32X4_MDACEN, AIC32X4_MDACEN);

		/* Switch on NADC Divider */
		snd_soc_update_bits(codec, AIC32X4_NADC,
				    AIC32X4_NADCEN, AIC32X4_NADCEN);

		/* Switch on MADC Divider */
		snd_soc_update_bits(codec, AIC32X4_MADC,
				    AIC32X4_MADCEN, AIC32X4_MADCEN);

		/* Switch on BCLK_N Divider */
		if (aic32x4_divs[aic32x4->rate_divs_index].blck_N)
			snd_soc_update_bits(codec, AIC32X4_BCLKN,
				AIC32X4_BCLKEN, AIC32X4_BCLKEN);
		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	case SND_SOC_BIAS_STANDBY:
		/* Switch off BCLK_N Divider */
		if (aic32x4_divs[aic32x4->rate_divs_index].blck_N)
			snd_soc_update_bits(codec, AIC32X4_BCLKN,
				AIC32X4_BCLKEN, 0);

		/* Switch off MADC Divider */
		snd_soc_update_bits(codec, AIC32X4_MADC,
				    AIC32X4_MADCEN, 0);

		/* Switch off NADC Divider */
		snd_soc_update_bits(codec, AIC32X4_NADC,
				    AIC32X4_NADCEN, 0);

		/* Switch off MDAC Divider */
		snd_soc_update_bits(codec, AIC32X4_MDAC,
				    AIC32X4_MDACEN, 0);

		/* Switch off NDAC Divider */
		snd_soc_update_bits(codec, AIC32X4_NDAC,
				    AIC32X4_NDACEN, 0);

		/* Switch off PLL */
		if (aic32x4_divs[aic32x4->rate_divs_index].p_val)
			snd_soc_update_bits(codec, AIC32X4_PLLPR,
				AIC32X4_PLLEN, 0);

		/* Switch off master clock */
		clk_disable_unprepare(aic32x4->mclk);
		break;
	case SND_SOC_BIAS_OFF:
		break;
	}
	codec->dapm.bias_level = level;
	return ret;
}

#define AIC32X4_RATES	SNDRV_PCM_RATE_8000_48000
#define AIC32X4_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE \
			 | SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S32_LE)

static const struct snd_soc_dai_ops aic32x4_ops = {
	.hw_params = aic32x4_hw_params,
	.digital_mute = aic32x4_mute,
	.set_fmt = aic32x4_set_dai_fmt,
	.set_sysclk = aic32x4_set_dai_sysclk,
};

static struct snd_soc_dai_driver aic32x4_dai = {
	.name = "tlv320aic32x4-hifi",
	.playback = {
		     .stream_name = "Playback",
		     .channels_min = 1,
		     .channels_max = 2,
		     .rates = AIC32X4_RATES,
		     .formats = AIC32X4_FORMATS,},
	.capture = {
		    .stream_name = "Capture",
		    .channels_min = 1,
		    .channels_max = 2,
		    .rates = AIC32X4_RATES,
		    .formats = AIC32X4_FORMATS,},
	.ops = &aic32x4_ops,
};

static int aic32x4_suspend(struct snd_soc_codec *codec)
{
	aic32x4_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static int aic32x4_resume(struct snd_soc_codec *codec)
{
	aic32x4_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	return 0;
}

static int aic32x4_probe(struct snd_soc_codec *codec)
{
	struct aic32x4_priv *aic32x4 = snd_soc_codec_get_drvdata(codec);
	u32 tmp_reg;

	pr_info("aic32x4: %s\n", __func__);

	if (gpio_is_valid(aic32x4->rstn_gpio)) {
		/* Hold reset down for at least 10nsec */
		gpio_set_value(aic32x4->rstn_gpio, 0);
		ndelay(10);
		gpio_set_value(aic32x4->rstn_gpio, 1);
	}

	snd_soc_write(codec, AIC32X4_RESET, 0x01);
	/* Required delay after reset */
	mdelay(1);

	/* Power platform configuration */
	if (aic32x4->power_cfg & AIC32X4_PWR_MICBIAS_2075_LDOIN) {
		snd_soc_write(codec, AIC32X4_MICBIAS, AIC32X4_MICBIAS_LDOIN |
						      AIC32X4_MICBIAS_2075V);
	}
	if (aic32x4->power_cfg & AIC32X4_PWR_AVDD_DVDD_WEAK_DISABLE)
		snd_soc_write(codec, AIC32X4_PWRCFG, AIC32X4_AVDDWEAKDISABLE);

	tmp_reg = (aic32x4->power_cfg & AIC32X4_PWR_AIC32X4_LDO_ENABLE) ?
			AIC32X4_LDOCTLEN : 0;
	snd_soc_write(codec, AIC32X4_LDOCTL, tmp_reg);

	tmp_reg = AIC32X4_HP_CMMODE;
	if (aic32x4->power_cfg & AIC32X4_PWR_CMMODE_LDOIN_RANGE_18_36)
		tmp_reg |= AIC32X4_LDOIN_18_36;
	if (aic32x4->power_cfg & AIC32X4_PWR_CMMODE_HP_LDOIN_POWERED)
		tmp_reg |= AIC32X4_LDOIN2HP;
	snd_soc_write(codec, AIC32X4_CMMODE, tmp_reg);

	/* Mic PGA routing */
	if (aic32x4->micpga_routing & AIC32X4_MICPGA_ROUTE_LMIC_IN2R_10K)
		snd_soc_write(codec, AIC32X4_LMICPGANIN,
				AIC32X4_LMICPGANIN_IN2R_10K);
	else
		snd_soc_write(codec, AIC32X4_LMICPGANIN,
				AIC32X4_LMICPGANIN_CM1L_10K);
	if (aic32x4->micpga_routing & AIC32X4_MICPGA_ROUTE_RMIC_IN1L_10K)
		snd_soc_write(codec, AIC32X4_RMICPGANIN,
				AIC32X4_RMICPGANIN_IN1L_10K);
	else
		snd_soc_write(codec, AIC32X4_RMICPGANIN,
				AIC32X4_RMICPGANIN_CM1R_10K);

	aic32x4_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	/*
	 * Workaround: for an unknown reason, the ADC needs to be powered up
	 * and down for the first capture to work properly. It seems related to
	 * a HW BUG or some kind of behavior not documented in the datasheet.
	 */
	tmp_reg = snd_soc_read(codec, AIC32X4_ADCSETUP);
	snd_soc_write(codec, AIC32X4_ADCSETUP, tmp_reg |
				AIC32X4_LADC_EN | AIC32X4_RADC_EN);
	snd_soc_write(codec, AIC32X4_ADCSETUP, tmp_reg);

	/* Turn MFP2 into GPIO mode and setout to High to mute AMP if present */
	snd_soc_write(codec, AIC32X4_DOUTCTL, MFP2_GPIO_ENABLE|MFP2_GPIO_HI);
	/* Store default state as muted */
	aic32x4->mfp_amp_muted = true;
	/* Disable SCLK/MFP3 */
	snd_soc_write(codec, AIC32X4_SCLKMFP, 0);
	/* Set the REF to 40ms */
	snd_soc_write(codec, AIC32X4_REF_PWRUP, REF_POWERUP_DELAY);

	return 0;
}

static int aic32x4_remove(struct snd_soc_codec *codec)
{
	aic32x4_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_aic32x4 = {
	.probe = aic32x4_probe,
	.remove = aic32x4_remove,
	.suspend = aic32x4_suspend,
	.resume = aic32x4_resume,
	.set_bias_level = aic32x4_set_bias_level,

	.controls = aic32x4_snd_controls,
	.num_controls = ARRAY_SIZE(aic32x4_snd_controls),
	.dapm_widgets = aic32x4_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aic32x4_dapm_widgets),
	.dapm_routes = aic32x4_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(aic32x4_dapm_routes),
};

static int aic32x4_parse_dt(struct aic32x4_priv *aic32x4,
		struct device_node *np)
{
	pr_info("aic32x4: %s\n", __func__);
	aic32x4->swapdacs = false;
	aic32x4->micpga_routing = 0;
	aic32x4->rstn_gpio = of_get_named_gpio(np, "reset-gpios", 0);
	aic32x4->power_cfg = AIC32X4_PWR_AVDD_DVDD_WEAK_DISABLE |
				AIC32X4_PWR_AIC32X4_LDO_ENABLE |
				AIC32X4_PWR_CMMODE_LDOIN_RANGE_18_36 |
				AIC32X4_PWR_CMMODE_HP_LDOIN_POWERED;

	return 0;
}

static void aic32x4_disable_regulators(struct aic32x4_priv *aic32x4)
{
	regulator_disable(aic32x4->supply_iov);

	if (!IS_ERR(aic32x4->supply_ldo))
		regulator_disable(aic32x4->supply_ldo);

	if (!IS_ERR(aic32x4->supply_dv))
		regulator_disable(aic32x4->supply_dv);

	if (!IS_ERR(aic32x4->supply_av))
		regulator_disable(aic32x4->supply_av);
}

static int aic32x4_setup_regulators(struct device *dev,
		struct aic32x4_priv *aic32x4)
{
	int ret = 0;

	aic32x4->supply_ldo = devm_regulator_get_optional(dev, "ldoin");
	aic32x4->supply_iov = devm_regulator_get(dev, "iov");
	aic32x4->supply_dv = devm_regulator_get_optional(dev, "dv");
	aic32x4->supply_av = devm_regulator_get_optional(dev, "av");

	/* Check if the regulator requirements are fulfilled */

	if (IS_ERR(aic32x4->supply_iov)) {
		dev_err(dev, "Missing supply 'iov'\n");
		return PTR_ERR(aic32x4->supply_iov);
	}

	if (IS_ERR(aic32x4->supply_ldo)) {
		if (PTR_ERR(aic32x4->supply_ldo) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		if (IS_ERR(aic32x4->supply_dv)) {
			dev_err(dev, "Missing supply 'dv' or 'ldoin'\n");
			return PTR_ERR(aic32x4->supply_dv);
		}
		if (IS_ERR(aic32x4->supply_av)) {
			dev_err(dev, "Missing supply 'av' or 'ldoin'\n");
			return PTR_ERR(aic32x4->supply_av);
		}
	} else {
		if (IS_ERR(aic32x4->supply_dv) &&
				PTR_ERR(aic32x4->supply_dv) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (IS_ERR(aic32x4->supply_av) &&
				PTR_ERR(aic32x4->supply_av) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	ret = regulator_enable(aic32x4->supply_iov);
	if (ret) {
		dev_err(dev, "Failed to enable regulator iov\n");
		return ret;
	}

	if (!IS_ERR(aic32x4->supply_ldo)) {
		ret = regulator_enable(aic32x4->supply_ldo);
		if (ret) {
			dev_err(dev, "Failed to enable regulator ldo\n");
			goto error_ldo;
		}
	}

	if (!IS_ERR(aic32x4->supply_dv)) {
		ret = regulator_enable(aic32x4->supply_dv);
		if (ret) {
			dev_err(dev, "Failed to enable regulator dv\n");
			goto error_dv;
		}
	}

	if (!IS_ERR(aic32x4->supply_av)) {
		ret = regulator_enable(aic32x4->supply_av);
		if (ret) {
			dev_err(dev, "Failed to enable regulator av\n");
			goto error_av;
		}
	}

	if (!IS_ERR(aic32x4->supply_ldo) && IS_ERR(aic32x4->supply_av))
		aic32x4->power_cfg |= AIC32X4_PWR_AIC32X4_LDO_ENABLE;

	return 0;

error_av:
	if (!IS_ERR(aic32x4->supply_dv))
		regulator_disable(aic32x4->supply_dv);

error_dv:
	if (!IS_ERR(aic32x4->supply_ldo))
		regulator_disable(aic32x4->supply_ldo);

error_ldo:
	regulator_disable(aic32x4->supply_iov);
	return ret;
}

static int aic32x4_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct aic32x4_pdata *pdata = i2c->dev.platform_data;
	struct aic32x4_priv *aic32x4;
	struct device_node *np = i2c->dev.of_node;
	int ret;

	pr_info("aic32x4: %s\n", __func__);

	aic32x4 = devm_kzalloc(&i2c->dev, sizeof(struct aic32x4_priv),
			       GFP_KERNEL);
	if (aic32x4 == NULL)
		return -ENOMEM;

	/* Default to stereo */
	aic32x4->channels = 2;
	/* Default to not ignore ramp up time of DAC */
	aic32x4->ignore_ramp = false;

	aic32x4->regmap = devm_regmap_init_i2c(i2c, &aic32x4_regmap);
	if (IS_ERR(aic32x4->regmap))
		return PTR_ERR(aic32x4->regmap);

	i2c_set_clientdata(i2c, aic32x4);

	if (pdata) {
		aic32x4->power_cfg = pdata->power_cfg;
		aic32x4->swapdacs = pdata->swapdacs;
		aic32x4->micpga_routing = pdata->micpga_routing;
		aic32x4->rstn_gpio = pdata->rstn_gpio;
	} else if (np) {
		ret = aic32x4_parse_dt(aic32x4, np);
		if (ret) {
			dev_err(&i2c->dev, "Failed to parse DT node\n");
			return ret;
		}
	} else {
		aic32x4->power_cfg = 0;
		aic32x4->swapdacs = false;
		aic32x4->micpga_routing = 0;
		aic32x4->rstn_gpio = -1;
	}

	aic32x4->mclk = devm_clk_get(&i2c->dev, "mclk");
	if (IS_ERR(aic32x4->mclk)) {
		dev_err(&i2c->dev, "Failed getting the mclk. The current implementation does not support the usage of this codec without mclk\n");
		return PTR_ERR(aic32x4->mclk);
	}

	if (gpio_is_valid(aic32x4->rstn_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, aic32x4->rstn_gpio,
				GPIOF_OUT_INIT_LOW, "tlv320aic32x4 rstn");
		if (ret != 0)
			return ret;
	}

	ret = snd_soc_register_codec(&i2c->dev,
			&soc_codec_dev_aic32x4, &aic32x4_dai, 1);
	if (ret) {
		dev_err(&i2c->dev, "Failed to register codec\n");
		return ret;
	}

	i2c_set_clientdata(i2c, aic32x4);

	pr_info("aic32x4: %s end\n", __func__);

	return 0;
}

static int aic32x4_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);
	return 0;
}

static const struct i2c_device_id aic32x4_i2c_id[] = {
	{ "tlv320aic32x4", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aic32x4_i2c_id);

static const struct of_device_id aic32x4_of_id[] = {
	{ .compatible = "ti,tlv320aic32x4", },
	{ /* senitel */ }
};
MODULE_DEVICE_TABLE(of, aic32x4_of_id);

static struct i2c_driver aic32x4_i2c_driver = {
	.driver = {
		.name = "tlv320aic32x4",
		.owner = THIS_MODULE,
		.of_match_table = aic32x4_of_id,
	},
	.probe =    aic32x4_i2c_probe,
	.remove =   aic32x4_i2c_remove,
	.id_table = aic32x4_i2c_id,
};

module_i2c_driver(aic32x4_i2c_driver);

MODULE_DESCRIPTION("ASoC tlv320aic32x4 codec driver");
MODULE_AUTHOR("Javier Martin <javier.martin@vista-silicon.com>");
MODULE_LICENSE("GPL");
