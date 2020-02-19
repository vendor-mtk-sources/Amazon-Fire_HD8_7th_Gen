/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
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


/*****************************************************************************
 *                     C O M P I L E R   F L A G S
 *****************************************************************************/


/*****************************************************************************
 *                E X T E R N A L   R E F E R E N C E S
 *****************************************************************************/

#include <linux/dma-mapping.h>
#include "AudDrv_Common.h"
#include "AudDrv_Def.h"
#include "AudDrv_Afe.h"
#include "AudDrv_Ana.h"
#include "AudDrv_Clk.h"
#include "AudDrv_Kernel.h"
#include "mt_soc_afe_control.h"
#include "mt_soc_digital_type.h"
#include "mt_soc_pcm_common.h"
#include <mt-plat/mt_gpt.h>


#define MAGIC_NUMBER 0xFFFFFFC0
static DEFINE_SPINLOCK(auddrv_I2S0dl1_lock);
static AFE_MEM_CONTROL_T *pI2S0dl1MemControl;
static struct snd_dma_buffer *Dl1_Playback_dma_buf;
static unsigned int mPlaybackSramState = SRAM_STATE_FREE;

/*
 *    function implementation
 */
static long long mtk_pcm_I2S0dl1_get_next_write_timestamp(void);
static int mtk_I2S0dl1_probe(struct platform_device *pdev);
static int mtk_pcm_I2S0dl1_close(struct snd_pcm_substream *substream);
static int mtk_asoc_pcm_I2S0dl1_new(struct snd_soc_pcm_runtime *rtd);
static int mtk_afe_I2S0dl1_probe(struct snd_soc_platform *platform);
#ifdef CONFIG_SND_I2S_MCLK
static int mI2S0dl1_hdoutput_control = true;
#else
static int mI2S0dl1_hdoutput_control;
#endif
static bool mPrepareDone;
static bool mCopyFlag;
static struct device *mDev;
static struct snd_pcm_substream *I2S0dl1_substream;
static unsigned long copy_count;
#define GPT6_CLOCK_COUNT_PER_SEC (13000000LL) /* 13MHz for GPT6 */
#define BYTES_OF_INT64 (8)

static const char * const I2S0dl1_HD_output[] = { "Off", "On" };

static const struct soc_enum Audio_I2S0dl1_Enum[] = {

	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(I2S0dl1_HD_output), I2S0dl1_HD_output),
};

static int Audio_I2S0dl1_timestamp_Get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	/* calculate the remain buffer for the timestamp */
	long long mI2S0dl1_next_write_timestamp;

	mI2S0dl1_next_write_timestamp = mtk_pcm_I2S0dl1_get_next_write_timestamp();
	ucontrol->value.integer64.value[0] = mI2S0dl1_next_write_timestamp;
	pr_debug("Audio_I2S0dl1_timestamp_Get = 0x%llx\n", mI2S0dl1_next_write_timestamp);
	return 0;
}

static int Audio_I2S0dl1_timestamp_Set(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("Audio_I2S0dl1_timestamp_Set do nothing\n");
	return 0;
}

static int Audio_I2S0dl1_hdoutput_Get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("Audio_I2S0dl1_hdoutput_Get = %d\n", mI2S0dl1_hdoutput_control);
	ucontrol->value.integer.value[0] = mI2S0dl1_hdoutput_control;
	return 0;
}

static int Audio_I2S0dl1_hdoutput_Set(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s()\n", __func__);
	if (ucontrol->value.enumerated.item[0] > ARRAY_SIZE(I2S0dl1_HD_output)) {
		pr_err("return -EINVAL\n");
		return -EINVAL;
	}

	mI2S0dl1_hdoutput_control = ucontrol->value.integer.value[0];

	if (GetMemoryPathEnable(Soc_Aud_Digital_Block_MEM_HDMI) == true) {
		pr_err("return HDMI enabled\n");
		return 0;
	}

	return 0;
}


static const struct snd_kcontrol_new Audio_snd_I2S0dl1_controls[] = {

	SOC_ENUM_EXT("Audio_I2S0dl1_hd_Switch", Audio_I2S0dl1_Enum[0], Audio_I2S0dl1_hdoutput_Get,
		     Audio_I2S0dl1_hdoutput_Set),
	SND_SOC_BYTES_EXT("Audio_I2S0dl1_get_timestamp", BYTES_OF_INT64, Audio_I2S0dl1_timestamp_Get,
		     Audio_I2S0dl1_timestamp_Set),
};

static struct snd_pcm_hardware mtk_I2S0dl1_hardware = {

	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SND_SOC_ADV_MT_FMTS,
	.rates = SOC_HIGH_USE_RATE,
	.rate_min = SOC_HIGH_USE_RATE_MIN,
	.rate_max = SOC_HIGH_USE_RATE_MAX,
	.channels_min = SOC_NORMAL_USE_CHANNELS_MIN,
	.channels_max = SOC_NORMAL_USE_CHANNELS_MAX,
	.buffer_bytes_max = SOC_NORMAL_USE_BUFFERSIZE_MAX,
	.period_bytes_max = SOC_NORMAL_USE_BUFFERSIZE_MAX,
	.periods_min = SOC_NORMAL_USE_PERIODS_MIN,
	.periods_max = SOC_NORMAL_USE_PERIODS_MAX,
	.fifo_size = 0,
};

static long long mtk_pcm_I2S0dl1_get_next_write_timestamp(void)
{
	kal_int32 HW_memory_index = 0;
	kal_int32 HW_Cur_ReadIdx = 0;
	kal_int32 Afe_consumed_bytes = 0;
	kal_int32 u4DataRemained = 0;
	AFE_BLOCK_T *Afe_Block = NULL;
	unsigned long flags = 0 ;
	long long timestamp = 0;
	long long timeForDataRemained = 0;
	unsigned int gpt_val[2] = {0, 0};
	unsigned int u4AdjustDataRemainedSample = 0;
	int tmp = 0;
	kal_int32 ValMcuIrq1Mon = 0;

	if (pI2S0dl1MemControl == NULL) {
		return -1;
	} else {
		Afe_Block = &pI2S0dl1MemControl->rBlock;
		if (Afe_Block == NULL)
			return -1;
	}

	spin_lock_irqsave(&pI2S0dl1MemControl->substream_lock, flags);
	PRINTK_AUD_DL1("%s Afe_Block->u4DMAReadIdx = 0x%x\n", __func__,
		Afe_Block->u4DMAReadIdx);

	if ((GetMemoryPathEnable(Soc_Aud_Digital_Block_MEM_DL1) == true)
					&& (I2S0dl1_substream != NULL)) {
		HW_Cur_ReadIdx = Afe_Get_Reg(AFE_DL1_CUR);
		ValMcuIrq1Mon = Afe_Get_Reg(AFE_IRQ1_MCU_CNT_MON);
		gpt_get_cnt(GPT6, gpt_val);
		if (HW_Cur_ReadIdx == 0) {
			PRINTK_AUDDRV("[Auddrv] HW_Cur_ReadIdx == 0\n");
			HW_Cur_ReadIdx = Afe_Block->pucPhysBufAddr;
		}

		HW_memory_index = (HW_Cur_ReadIdx - Afe_Block->pucPhysBufAddr);

		if (HW_memory_index >= Afe_Block->u4DMAReadIdx)
			Afe_consumed_bytes = HW_memory_index - Afe_Block->u4DMAReadIdx;
		else {
			Afe_consumed_bytes = Afe_Block->u4BufferSize + HW_memory_index
				- Afe_Block->u4DMAReadIdx;
		}

		Afe_consumed_bytes = Align64ByteSize(Afe_consumed_bytes);

		u4DataRemained = Afe_Block->u4DataRemained - Afe_consumed_bytes;

		spin_unlock_irqrestore(&pI2S0dl1MemControl->substream_lock, flags);

		/* calculating timestamp with GPT value and u4DataRemained */
		/* Default format is 16bit, 2-channels, 48kHz output */
		/* GPT6 is a 13Mhz counter */
		/* TODO 1. refine u4DataRemained */

		timestamp = gpt_val[1];
		timestamp = timestamp<<32;
		timestamp |= gpt_val[0];

		tmp = audio_bytes_to_frame(I2S0dl1_substream, u4DataRemained);
		tmp = tmp - ValMcuIrq1Mon;
		/* Interrupt period = 1/4 kernel buffer. */
		/* ValMcuIrq1Mon : real remaided samples in current half buffer. */
		/* u4DataRemained>>2(16tbit, 2-channels): Remained samples
		  count by current pointer(still some samples in prefetch buffer)*/
		/* if tmp < 0 , it means no extra data except this half buffer */
		/* else there is still data and we can count  */
		/*  <-copy_count->               |<------------IRQ------------>| */
		/* |--------------|--------------|--------------|--------------| */
		/*                                   |<--  ValMcuIrq1Mon  ---->| */
		/*                                                               */
		/*CASE1 : There is no data in another half buffer                */
		/* |                             |   |<--  ValMcuIrq1Mon  ---->| */
		/*                                                               */
		/*CASE2 :                                                        */
		/*   There is 1/4buffer(copy_count)data in another half buffer   */
		/* |XXcopy_countXX|              |   |<--  ValMcuIrq1Mon  ---->| */
		/*                                                               */
		/*CASE3 :                                                        */
		/*  There is 1/4buffer(2*copy_count)data in another half buffer  */
		/* |XXcopy_countXX|XXcopy_countXX|   |<--  ValMcuIrq1Mon  ---->| */
		/*                                                               */
		u4AdjustDataRemainedSample = ValMcuIrq1Mon;
		if (tmp >= 0) {
			/*u4DataRemained larger than irq monitor, it mean there are*/
			/*extra data , and need to add extra data                   */
			while (tmp > 0) {
				u4AdjustDataRemainedSample += copy_count;
				tmp -= copy_count;
			}
		} else {
			/*u4DataRemained less than irq monitor, check if the delta is */
			/* only pre-fetch buffer(192 bytes), if not, need to calculate*/
			/* real data by copy_count & the difference                   */
			tmp = tmp + audio_bytes_to_frame(I2S0dl1_substream, 256) ;
			while (tmp <= 0) {
				u4AdjustDataRemainedSample -= copy_count;
				tmp += copy_count;
			}
		}

		timeForDataRemained =
			GPT6_CLOCK_COUNT_PER_SEC * u4AdjustDataRemainedSample /
			I2S0dl1_substream->runtime->rate;

		PRINTK_AUD_DL1(
			"mon=%d,rem=%d,adj=%d,timeRem=%lld,ts0=%lld,ts=%lld\n",
			ValMcuIrq1Mon, u4DataRemained,
			u4AdjustDataRemainedSample, timeForDataRemained,
			timestamp, timestamp + timeForDataRemained);

		timestamp += timeForDataRemained;
		return timestamp;
	}

	/* should not happen. */
	pr_err("%s: MEM path to DL1 isn't enable, get timestamp=-1", __func__);
	spin_unlock_irqrestore(&pI2S0dl1MemControl->substream_lock, flags);
	return -1;
}

static int mtk_pcm_I2S0dl1_stop(struct snd_pcm_substream *substream)
{
	/* AFE_BLOCK_T *Afe_Block = &(pI2S0dl1MemControl->rBlock); */

	pr_debug("%s\n", __func__);

	SetIrqEnable(Soc_Aud_IRQ_MCU_MODE_IRQ1_MCU_MODE, false);
	SetMemoryPathEnable(Soc_Aud_Digital_Block_MEM_DL1, false);

	/* here start digital part */
#ifndef BCM_SCO_I2S
	SetConnection(Soc_Aud_InterCon_DisConnect, Soc_Aud_InterConnectionInput_I05,
		      Soc_Aud_InterConnectionOutput_O00);
	SetConnection(Soc_Aud_InterCon_DisConnect, Soc_Aud_InterConnectionInput_I06,
		      Soc_Aud_InterConnectionOutput_O01);
#endif
	SetConnection(Soc_Aud_InterCon_DisConnect, Soc_Aud_InterConnectionInput_I05,
		      Soc_Aud_InterConnectionOutput_O03);
	SetConnection(Soc_Aud_InterCon_DisConnect, Soc_Aud_InterConnectionInput_I06,
		      Soc_Aud_InterConnectionOutput_O04);

	ClearMemBlock(Soc_Aud_Digital_Block_MEM_DL1);
	mCopyFlag = false;
	return 0;
}

static snd_pcm_uframes_t mtk_pcm_I2S0dl1_pointer(struct snd_pcm_substream *substream)
{
	kal_int32 HW_memory_index = 0;
	kal_int32 HW_Cur_ReadIdx = 0;
	kal_uint32 Frameidx = 0;
	kal_int32 Afe_consumed_bytes = 0;
	AFE_BLOCK_T *Afe_Block = &pI2S0dl1MemControl->rBlock;
	unsigned long flags;

	spin_lock_irqsave(&pI2S0dl1MemControl->substream_lock, flags);
	PRINTK_AUD_DL1("%s Afe_Block->u4DMAReadIdx = 0x%x\n", __func__,
		Afe_Block->u4DMAReadIdx);

	if (GetMemoryPathEnable(Soc_Aud_Digital_Block_MEM_DL1)) {
		HW_Cur_ReadIdx = Afe_Get_Reg(AFE_DL1_CUR);
		if (HW_Cur_ReadIdx == 0) {
			PRINTK_AUDDRV("[Auddrv] HW_Cur_ReadIdx == 0\n");
			HW_Cur_ReadIdx = Afe_Block->pucPhysBufAddr;
		}

		HW_memory_index = (HW_Cur_ReadIdx - Afe_Block->pucPhysBufAddr);

		if (HW_memory_index >= Afe_Block->u4DMAReadIdx) {
			Afe_consumed_bytes = HW_memory_index - Afe_Block->u4DMAReadIdx;
		} else {
			Afe_consumed_bytes = Afe_Block->u4BufferSize + HW_memory_index
				- Afe_Block->u4DMAReadIdx;
		}

		Afe_consumed_bytes = Align64ByteSize(Afe_consumed_bytes);

		Afe_Block->u4DataRemained -= Afe_consumed_bytes;
		Afe_Block->u4DMAReadIdx += Afe_consumed_bytes;
		Afe_Block->u4DMAReadIdx %= Afe_Block->u4BufferSize;

		PRINTK_AUD_DL1
			("[Auddrv] HW_Cur_ReadIdx = 0x%x, HW_memory_index = 0x%x, Afe_consumed_bytes = 0x%x\n",
			HW_Cur_ReadIdx, HW_memory_index, Afe_consumed_bytes);

		spin_unlock_irqrestore(&pI2S0dl1MemControl->substream_lock, flags);

		return audio_bytes_to_frame(substream, Afe_Block->u4DMAReadIdx);
	}

	Frameidx = audio_bytes_to_frame(substream, Afe_Block->u4DMAReadIdx);
	spin_unlock_irqrestore(&pI2S0dl1MemControl->substream_lock, flags);
	return Frameidx;
}


static void SetDL1Buffer(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	AFE_BLOCK_T *pblock = &pI2S0dl1MemControl->rBlock;

	pblock->pucPhysBufAddr = runtime->dma_addr;
	pblock->pucVirtBufAddr = runtime->dma_area;
	pblock->u4BufferSize = runtime->dma_bytes;
	pblock->u4SampleNumMask = 0x001f;	/* 32 byte align */
	pblock->u4WriteIdx = 0;
	pblock->u4DMAReadIdx = 0;
	pblock->u4DataRemained = 0;
	pblock->u4fsyncflag = false;
	pblock->uResetFlag = true;
	pr_debug("SetDL1Buffer u4BufferSize = %d, pucVirtBufAddr = %p, pucPhysBufAddr = 0x%x\n",
	       pblock->u4BufferSize, pblock->pucVirtBufAddr, pblock->pucPhysBufAddr);
	/* set dram address top hardware */
	Afe_Set_Reg(AFE_DL1_BASE, pblock->pucPhysBufAddr, 0xffffffff);
	Afe_Set_Reg(AFE_DL1_END, pblock->pucPhysBufAddr + (pblock->u4BufferSize - 1), 0xffffffff);
	memset((void *)pblock->pucVirtBufAddr, 0, pblock->u4BufferSize);

}

static int mtk_pcm_I2S0dl1_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *hw_params)
{
	int ret = 0;

	substream->runtime->dma_bytes = params_buffer_bytes(hw_params);
	/* pr_debug("mtk_pcm_hw_params dma_bytes = %d\n",substream->runtime->dma_bytes); */
#if 1
	if (mPlaybackSramState == SRAM_STATE_PLAYBACKFULL) {
		/* substream->runtime->dma_bytes = AFE_INTERNAL_SRAM_SIZE; */
		substream->runtime->dma_area = (unsigned char *)Get_Afe_SramBase_Pointer();
		substream->runtime->dma_addr = AFE_INTERNAL_SRAM_PHY_BASE;
		AudDrv_Allocate_DL1_Buffer(mDev, substream->runtime->dma_bytes);
	} else {
		substream->runtime->dma_bytes = params_buffer_bytes(hw_params);
		substream->runtime->dma_area = Dl1_Playback_dma_buf->area;
		substream->runtime->dma_addr = Dl1_Playback_dma_buf->addr;
		SetDL1Buffer(substream, hw_params);
	}
#else				/* old */
	/* here to allcoate sram to hardware --------------------- */
	AudDrv_Allocate_mem_Buffer(Soc_Aud_Digital_Block_MEM_DL1, substream->runtime->dma_bytes);
#ifdef AUDIO_MEMORY_SRAM
	/* substream->runtime->dma_bytes = AFE_INTERNAL_SRAM_SIZE; */
	substream->runtime->dma_area = (unsigned char *)Get_Afe_SramBase_Pointer();
	substream->runtime->dma_addr = AFE_INTERNAL_SRAM_PHY_BASE;
#else
	pI2S0dl1MemControl = Get_Mem_ControlT(Soc_Aud_Digital_Block_MEM_DL1);
	Afe_Block = &pI2S0dl1MemControl->rBlock;

	substream->runtime->dma_area = (unsigned char *)Afe_Block->pucVirtBufAddr;
	substream->runtime->dma_addr = Afe_Block->pucPhysBufAddr;
#endif
	/* ------------------------------------------------------- */
#endif
	pr_debug("dma_bytes = %zu, dma_area = %p, dma_addr = 0x%lx\n",
	       substream->runtime->dma_bytes, substream->runtime->dma_area,
	       (long)substream->runtime->dma_addr);

	return ret;
}

static int mtk_pcm_I2S0dl1_hw_free(struct snd_pcm_substream *substream)
{
	return 0;
}

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {

	.count = ARRAY_SIZE(soc_high_supported_sample_rates),
	.list = soc_high_supported_sample_rates,
	.mask = 0,
};

static int mtk_pcm_I2S0dl1_open(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;

	I2S0dl1_substream = substream;

	AfeControlSramLock();
	if (GetSramState() == SRAM_STATE_FREE) {
		mtk_I2S0dl1_hardware.buffer_bytes_max = GetPLaybackSramFullSize();
		mPlaybackSramState = SRAM_STATE_PLAYBACKFULL;
		SetSramState(mPlaybackSramState);
	} else {
		mtk_I2S0dl1_hardware.buffer_bytes_max = GetPLaybackDramSize();
		mPlaybackSramState = SRAM_STATE_PLAYBACKDRAM;
	}
	AfeControlSramUnLock();

	if (mPlaybackSramState == SRAM_STATE_PLAYBACKDRAM)
		AudDrv_Emi_Clk_On();

	pr_debug("mtk_I2S0dl1_hardware.buffer_bytes_max = %zu, mPlaybackSramState = %d\n",
	       mtk_I2S0dl1_hardware.buffer_bytes_max, mPlaybackSramState);
	runtime->hw = mtk_I2S0dl1_hardware;

	AudDrv_ANA_Clk_On();
	AudDrv_Clk_On();
	memcpy((void *)(&(runtime->hw)), (void *)&mtk_I2S0dl1_hardware,
	       sizeof(struct snd_pcm_hardware));
	pI2S0dl1MemControl = Get_Mem_ControlT(Soc_Aud_Digital_Block_MEM_DL1);

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &constraints_sample_rates);

	if (ret < 0)
		pr_warn("snd_pcm_hw_constraint_integer failed\n");

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		pr_debug("SNDRV_PCM_STREAM_PLAYBACK mtkalsa_I2S0dl1_playback_constraints\n");
	else
		pr_debug("SNDRV_PCM_STREAM_CAPTURE mtkalsa_I2S0dl1_playback_constraints\n");

	if (ret < 0) {
		pr_warn("ret < 0 mtk_pcm_I2S0dl1_close\n");
		mtk_pcm_I2S0dl1_close(substream);
		return ret;
	}

	/* pr_debug("mtk_pcm_I2S0dl1_open return\n"); */
	return 0;
}

static int mtk_pcm_I2S0dl1_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	I2S0dl1_substream = NULL;

	pr_debug("%s\n", __func__);

	if (mPrepareDone == true) {
		/* stop DAC output */
		SetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_DAC, false);

		if (GetI2SDacEnable() == false)
			SetI2SDacEnable(false);

#ifndef BCM_SCO_I2S
		/* stop I2S output */
		SetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_2, false);

		if (GetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_2) == false)
			Afe_Set_Reg(AFE_I2S_CON3, 0x0, 0x1);
#endif

		RemoveMemifSubStream(Soc_Aud_Digital_Block_MEM_DL1, substream);

		EnableAfe(false);

		if (mI2S0dl1_hdoutput_control == true) {
			pr_debug("%s mI2S0dl1_hdoutput_control == %d\n", __func__,
			       mI2S0dl1_hdoutput_control);

			EnableI2SDivPower(AUDIO_APLL12_DIV2, false);
			EnableI2SDivPower(AUDIO_APLL12_DIV4, false);	/* Todo do we need open I2S3? */

			EnableApll(runtime->rate, false);
			EnableApllTuner(runtime->rate, false);
		}

		mPrepareDone = false;
	}

	if (mPlaybackSramState == SRAM_STATE_PLAYBACKDRAM)
		AudDrv_Emi_Clk_Off();

	AfeControlSramLock();
	ClearSramState(mPlaybackSramState);
	mPlaybackSramState = GetSramState();
	AfeControlSramUnLock();
	AudDrv_Clk_Off();
	AudDrv_ANA_Clk_Off();
	return 0;
}

static int mtk_pcm_I2S0dl1_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	uint32 MclkDiv3;
	uint32 u32AudioI2S = 0;
	bool mI2SWLen;

	if (mPrepareDone == false) {
		pr_debug("%s format = %d SNDRV_PCM_FORMAT_S32_LE = %d SNDRV_PCM_FORMAT_U32_LE = %d\n",
		       __func__, runtime->format, SNDRV_PCM_FORMAT_S32_LE, SNDRV_PCM_FORMAT_U32_LE);
		SetMemifSubStream(Soc_Aud_Digital_Block_MEM_DL1, substream);

		if (runtime->format == SNDRV_PCM_FORMAT_S32_LE
		    || runtime->format == SNDRV_PCM_FORMAT_U32_LE) {
#ifdef CONFIG_MTK_ALIGN_24BIT_DATA_8BIT_0
			SetMemIfFetchFormatPerSample(Soc_Aud_Digital_Block_MEM_DL1,
						     AFE_WLEN_32_BIT_ALIGN_24BIT_DATA_8BIT_0);
			SetMemIfFetchFormatPerSample(Soc_Aud_Digital_Block_MEM_DL2,
						     AFE_WLEN_32_BIT_ALIGN_24BIT_DATA_8BIT_0);
#else
			SetMemIfFetchFormatPerSample(Soc_Aud_Digital_Block_MEM_DL1,
						     AFE_WLEN_32_BIT_ALIGN_8BIT_0_24BIT_DATA);
			SetMemIfFetchFormatPerSample(Soc_Aud_Digital_Block_MEM_DL2,
						     AFE_WLEN_32_BIT_ALIGN_8BIT_0_24BIT_DATA);
#endif
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O03);
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O04);
#ifndef BCM_SCO_I2S
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O00);
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O01);
#endif
			mI2SWLen = Soc_Aud_I2S_WLEN_WLEN_32BITS;
		} else {
			SetMemIfFetchFormatPerSample(Soc_Aud_Digital_Block_MEM_DL1,
						     AFE_WLEN_16_BIT);
			SetMemIfFetchFormatPerSample(Soc_Aud_Digital_Block_MEM_DL2,
						     AFE_WLEN_16_BIT);
#ifdef CONFIG_MTK_16BIT_IN_24BIT_OUT
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O03);
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O04);
#ifndef BCM_SCO_I2S
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O00);
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_24BIT,
						  Soc_Aud_InterConnectionOutput_O01);
#endif
			mI2SWLen = Soc_Aud_I2S_WLEN_WLEN_32BITS;
#else
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_16BIT,
						  Soc_Aud_InterConnectionOutput_O03);
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_16BIT,
						  Soc_Aud_InterConnectionOutput_O04);
#ifndef BCM_SCO_I2S
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_16BIT,
						  Soc_Aud_InterConnectionOutput_O00);
			SetoutputConnectionFormat(OUTPUT_DATA_FORMAT_16BIT,
						  Soc_Aud_InterConnectionOutput_O01);
#endif
			mI2SWLen = Soc_Aud_I2S_WLEN_WLEN_16BITS;
#endif
		}

		SetSampleRate(Soc_Aud_Digital_Block_MEM_I2S, runtime->rate);

		/* I2S out Setting */
		u32AudioI2S = SampleRateTransform(runtime->rate) << 8;
		u32AudioI2S |= Soc_Aud_I2S_FORMAT_I2S << 3;	/* us3 I2s format */
		u32AudioI2S |= Soc_Aud_I2S_WLEN_WLEN_32BITS << 1;	/* 32bit */

		if (mI2S0dl1_hdoutput_control == true) {
			pr_debug("%s mI2S0dl1_hdoutput_control == %d\n", __func__,
			       mI2S0dl1_hdoutput_control);
			/* open apll */
			EnableApll(runtime->rate, true);
			EnableApllTuner(runtime->rate, true);

			MclkDiv3 = SetCLkMclk(Soc_Aud_I2S1, runtime->rate);	/* select I2S */
#ifdef CONFIG_SND_I2S_MCLK
			MclkDiv3 = SetCLkMclk(Soc_Aud_I2S2, runtime->rate);
#else
#ifndef BCM_SCO_I2S
			MclkDiv3 = SetCLkMclk(Soc_Aud_I2S3, runtime->rate);	/* Todo do we need open I2S3? */
#endif
#endif

			/* Following wil not affect hw. because I2S0-I2S3 BCK is generated by APLL1 DIV0 APLL2 Div0 */
			SetCLkBclk(MclkDiv3, runtime->rate, runtime->channels,
				   Soc_Aud_I2S_WLEN_WLEN_32BITS);

			EnableI2SDivPower(AUDIO_APLL12_DIV2, true);
#ifndef CONFIG_SND_I2S_MCLK
			EnableI2SDivPower(AUDIO_APLL12_DIV4, true);
#endif

			u32AudioI2S |= Soc_Aud_LOW_JITTER_CLOCK << 12;	/* Low jitter mode */

		} else
			u32AudioI2S &= ~(Soc_Aud_LOW_JITTER_CLOCK << 12);

#ifndef BCM_SCO_I2S
		if (GetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_2) == false) {
			SetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_2, true);
			Afe_Set_Reg(AFE_I2S_CON3, u32AudioI2S | 1, AFE_MASK_ALL);
		} else
			SetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_2, true);
#endif

		/* start I2S DAC out */
		if (GetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_DAC) == false) {
			SetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_DAC, true);
			SetI2SDacOut(substream->runtime->rate, mI2S0dl1_hdoutput_control, mI2SWLen);
			SetI2SDacEnable(true);
		} else
			SetMemoryPathEnable(Soc_Aud_Digital_Block_I2S_OUT_DAC, true);

		/* here to set interrupt_distributor */
		SetIrqMcuCounter(Soc_Aud_IRQ_MCU_MODE_IRQ1_MCU_MODE, runtime->period_size);
		SetIrqMcuSampleRate(Soc_Aud_IRQ_MCU_MODE_IRQ1_MCU_MODE, runtime->rate);

		EnableAfe(true);
		mPrepareDone = true;
	}
	return 0;
}

static int mtk_pcm_I2S0dl1_start(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	pr_debug("%s\n", __func__);
	/* here start digital part */

#ifndef BCM_SCO_I2S
	SetConnection(Soc_Aud_InterCon_Connection, Soc_Aud_InterConnectionInput_I05,
		      Soc_Aud_InterConnectionOutput_O00);
	SetConnection(Soc_Aud_InterCon_Connection, Soc_Aud_InterConnectionInput_I06,
		      Soc_Aud_InterConnectionOutput_O01);
#endif
	SetConnection(Soc_Aud_InterCon_Connection, Soc_Aud_InterConnectionInput_I05,
		      Soc_Aud_InterConnectionOutput_O03);
	SetConnection(Soc_Aud_InterCon_Connection, Soc_Aud_InterConnectionInput_I06,
		      Soc_Aud_InterConnectionOutput_O04);

	SetIrqEnable(Soc_Aud_IRQ_MCU_MODE_IRQ1_MCU_MODE, true);

	SetSampleRate(Soc_Aud_Digital_Block_MEM_DL1, runtime->rate);
	SetChannels(Soc_Aud_Digital_Block_MEM_DL1, runtime->channels);
	SetMemoryPathEnable(Soc_Aud_Digital_Block_MEM_DL1, true);

	EnableAfe(true);

	return 0;
}

static int mtk_pcm_I2S0dl1_trigger(struct snd_pcm_substream *substream, int cmd)
{
	/* printk("mtk_pcm_I2S0dl1_trigger cmd = %d\n", cmd); */

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return mtk_pcm_I2S0dl1_start(substream);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return mtk_pcm_I2S0dl1_stop(substream);
	}
	return -EINVAL;
}

static int mtk_pcm_I2S0dl1_copy(struct snd_pcm_substream *substream,
				int channel, snd_pcm_uframes_t pos,
				void __user *dst, snd_pcm_uframes_t count)
{
	AFE_BLOCK_T *Afe_Block = NULL;
	int copy_size = 0, Afe_WriteIdx_tmp;
	unsigned long flags;
	char *data_w_ptr = (char *)dst;

	PRINTK_AUD_DL1("mtk_pcm_copy pos = %lu count = %lu\n", pos, count);

	if (!mCopyFlag) {
		copy_count = count;
		mCopyFlag = true;
	}
	/* get total bytes to copy */
	count = audio_frame_to_bytes(substream, count);

	/* check which memif nned to be write */
	Afe_Block = &pI2S0dl1MemControl->rBlock;

	/* handle for buffer management */

	PRINTK_AUD_DL1("WriteIdx = 0x%x, ReadIdx = 0x%x, DataRemained = 0x%x\n",
		       Afe_Block->u4WriteIdx, Afe_Block->u4DMAReadIdx, Afe_Block->u4DataRemained);

	if (Afe_Block->u4BufferSize == 0) {
		pr_err("u4BufferSize = 0, Error!!!\n");
		return 0;
	}

	/* free space of the buffer */
	spin_lock_irqsave(&auddrv_I2S0dl1_lock, flags);
	copy_size = Afe_Block->u4BufferSize - Afe_Block->u4DataRemained;
	spin_unlock_irqrestore(&auddrv_I2S0dl1_lock, flags);

	if (count <= copy_size) {
		if (copy_size < 0)
			copy_size = 0;
		else
			copy_size = count;
	}

	copy_size = Align64ByteSize(copy_size);
	PRINTK_AUD_DL1("copy_size = 0x%x, count = 0x%x\n", copy_size, (unsigned int)count);

	if (copy_size != 0) {
		spin_lock_irqsave(&auddrv_I2S0dl1_lock, flags);
		Afe_WriteIdx_tmp = Afe_Block->u4WriteIdx;
		spin_unlock_irqrestore(&auddrv_I2S0dl1_lock, flags);

		if (Afe_WriteIdx_tmp + copy_size < Afe_Block->u4BufferSize) {	/* copy once */
			if (!access_ok(VERIFY_READ, data_w_ptr, copy_size)) {
				pr_warn("0 ptr invalid data_w_ptr = %p, size = %d", data_w_ptr,
					      copy_size);
				pr_warn(" u4BufferSize = %d, u4DataRemained = %d\n",
					      Afe_Block->u4BufferSize, Afe_Block->u4DataRemained);
			} else {
				PRINTK_AUD_DL1
				("memcpy Afe_Block->pucVirtBufAddr+Afe_WriteIdx=%p, data_w_ptr=%p, copy_size=%x\n",
				Afe_Block->pucVirtBufAddr + Afe_WriteIdx_tmp, data_w_ptr, copy_size);

				if (copy_from_user((Afe_Block->pucVirtBufAddr + Afe_WriteIdx_tmp),
					data_w_ptr, copy_size)) {
					PRINTK_AUDDRV("Fail copy from user\n");
					return -1;
				}
			}

			spin_lock_irqsave(&auddrv_I2S0dl1_lock, flags);
			Afe_Block->u4DataRemained += copy_size;
			Afe_Block->u4WriteIdx = Afe_WriteIdx_tmp + copy_size;
			Afe_Block->u4WriteIdx %= Afe_Block->u4BufferSize;
			spin_unlock_irqrestore(&auddrv_I2S0dl1_lock, flags);
			data_w_ptr += copy_size;
			count -= copy_size;

			PRINTK_AUD_DL1
				("finish 1, copy_size:%x, WriteIdx:%x, ReadIdx:%x, DataRemained:%x, count:%x\n",
				copy_size, Afe_Block->u4WriteIdx, Afe_Block->u4DMAReadIdx,
				Afe_Block->u4DataRemained, (unsigned int)count);

		} else {	/* copy twice */
			kal_uint32 size_1 = 0, size_2 = 0;

			size_1 = Align64ByteSize((Afe_Block->u4BufferSize - Afe_WriteIdx_tmp));
			size_2 = Align64ByteSize((copy_size - size_1));
			PRINTK_AUD_DL1("size_1 = 0x%x, size_2 = 0x%x\n", size_1, size_2);
			if (!access_ok(VERIFY_READ, data_w_ptr, size_1)) {
				pr_warn("1 ptr invalid, data_w_ptr = %p, size_1 = %d,", data_w_ptr,
				       size_1);
				pr_warn(" u4BufferSize = %d, u4DataRemained = %d\n",
				       Afe_Block->u4BufferSize, Afe_Block->u4DataRemained);
			} else {
				PRINTK_AUD_DL1
				("mcmcpy, Afe_Block->pucVirtBufAddr+Afe_WriteIdx=%p, data_w_ptr=%p, size_1=%x\n",
				Afe_Block->pucVirtBufAddr + Afe_WriteIdx_tmp, data_w_ptr, size_1);

				if ((copy_from_user((Afe_Block->pucVirtBufAddr + Afe_WriteIdx_tmp),
					data_w_ptr, size_1))) {
					PRINTK_AUDDRV("Fail 1 copy from user\n");
					return -1;
				}
			}
			spin_lock_irqsave(&auddrv_I2S0dl1_lock, flags);
			Afe_Block->u4DataRemained += size_1;
			Afe_Block->u4WriteIdx = Afe_WriteIdx_tmp + size_1;
			Afe_Block->u4WriteIdx %= Afe_Block->u4BufferSize;
			Afe_WriteIdx_tmp = Afe_Block->u4WriteIdx;
			spin_unlock_irqrestore(&auddrv_I2S0dl1_lock, flags);

			if (!access_ok(VERIFY_READ, data_w_ptr + size_1, size_2)) {
				PRINTK_AUDDRV("2 ptr invalid, data_w_ptr = %p, size_1 = %d, size_2 = %d,",
					      data_w_ptr, size_1, size_2);
				PRINTK_AUDDRV(" u4BufferSize = %d, u4DataRemained = %d\n",
					      Afe_Block->u4BufferSize, Afe_Block->u4DataRemained);
			} else {
				PRINTK_AUD_DL1
				("mcmcpy, Afe_Block->pucVirtBufAddr+Afe_WriteIdx=%p, data_w_ptr+size_1=%p, size_2=%x\n",
				Afe_Block->pucVirtBufAddr + Afe_WriteIdx_tmp, data_w_ptr + size_1, size_2);

				if ((copy_from_user((Afe_Block->pucVirtBufAddr + Afe_WriteIdx_tmp),
				      (data_w_ptr + size_1), size_2))) {
					PRINTK_AUDDRV("AudDrv_write Fail 2 copy from user\n");
					return -1;
				}
			}
			spin_lock_irqsave(&auddrv_I2S0dl1_lock, flags);

			Afe_Block->u4DataRemained += size_2;
			Afe_Block->u4WriteIdx = Afe_WriteIdx_tmp + size_2;
			Afe_Block->u4WriteIdx %= Afe_Block->u4BufferSize;
			spin_unlock_irqrestore(&auddrv_I2S0dl1_lock, flags);
			count -= copy_size;
			data_w_ptr += copy_size;

			PRINTK_AUD_DL1
				("finish 2, copy size:%x, WriteIdx:%x, ReadIdx:%x, DataRemained:%x\n",
				copy_size, Afe_Block->u4WriteIdx, Afe_Block->u4DMAReadIdx,
				Afe_Block->u4DataRemained);
		}
	}
	PRINTK_AUD_DL1("pcm_copy return\n");
	return 0;
}

static int mtk_pcm_I2S0dl1_silence(struct snd_pcm_substream *substream,
				   int channel, snd_pcm_uframes_t pos, snd_pcm_uframes_t count)
{
	pr_debug("%s\n", __func__);
	/* do nothing */
	return 0;
}

static void *dummy_page[2];

static struct page *mtk_I2S0dl1_pcm_page(struct snd_pcm_substream *substream, unsigned long offset)
{
	pr_debug("%s\n", __func__);
	return virt_to_page(dummy_page[substream->stream]);	/* the same page */
}

static struct snd_pcm_ops mtk_I2S0dl1_ops = {

	.open = mtk_pcm_I2S0dl1_open,
	.close = mtk_pcm_I2S0dl1_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = mtk_pcm_I2S0dl1_hw_params,
	.hw_free = mtk_pcm_I2S0dl1_hw_free,
	.prepare = mtk_pcm_I2S0dl1_prepare,
	.trigger = mtk_pcm_I2S0dl1_trigger,
	.pointer = mtk_pcm_I2S0dl1_pointer,
	.copy = mtk_pcm_I2S0dl1_copy,
	.silence = mtk_pcm_I2S0dl1_silence,
	.page = mtk_I2S0dl1_pcm_page,
};

static struct snd_soc_platform_driver mtk_I2S0dl1_soc_platform = {

	.ops = &mtk_I2S0dl1_ops,
	.pcm_new = mtk_asoc_pcm_I2S0dl1_new,
	.probe = mtk_afe_I2S0dl1_probe,
};

static int mtk_I2S0dl1_probe(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	if (pdev->dev.of_node)
		dev_set_name(&pdev->dev, "%s", MT_SOC_I2S0DL1_PCM);

	pr_debug("%s: dev name %s\n", __func__, dev_name(&pdev->dev));

	mDev = &pdev->dev;
	return snd_soc_register_platform(&pdev->dev, &mtk_I2S0dl1_soc_platform);
}

static int mtk_asoc_pcm_I2S0dl1_new(struct snd_soc_pcm_runtime *rtd)
{
	int ret = 0;

	pr_debug("%s\n", __func__);
	return ret;
}


static int mtk_afe_I2S0dl1_probe(struct snd_soc_platform *platform)
{
	pr_debug("mtk_afe_I2S0dl1_probe\n");
	snd_soc_add_platform_controls(platform, Audio_snd_I2S0dl1_controls,
				      ARRAY_SIZE(Audio_snd_I2S0dl1_controls));
	/* allocate dram */
	AudDrv_Allocate_mem_Buffer(platform->dev, Soc_Aud_Digital_Block_MEM_DL1,
				   Dl1_MAX_BUFFER_SIZE);
	Dl1_Playback_dma_buf = Get_Mem_Buffer(Soc_Aud_Digital_Block_MEM_DL1);
	return 0;
}

static int mtk_I2S0dl1_remove(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id mt_soc_pcm_dl1_i2s0Dl1_of_ids[] = {

	{.compatible = "mediatek,mt8163-soc-pcm-dl1-i2s0Dl1",},
	{}
};
#endif

static struct platform_driver mtk_I2S0dl1_driver = {

	.driver = {
		   .name = MT_SOC_I2S0DL1_PCM,
		   .owner = THIS_MODULE,
#ifdef CONFIG_OF
		   .of_match_table = mt_soc_pcm_dl1_i2s0Dl1_of_ids,
#endif
		   },
	.probe = mtk_I2S0dl1_probe,
	.remove = mtk_I2S0dl1_remove,
};

#ifndef CONFIG_OF
static struct platform_device *soc_mtkI2S0dl1_dev;
#endif

static int __init mtk_I2S0dl1_soc_platform_init(void)
{
	int ret;

	pr_debug("%s\n", __func__);
#ifndef CONFIG_OF
	soc_mtkI2S0dl1_dev = platform_device_alloc(MT_SOC_I2S0DL1_PCM, -1);

	if (!soc_mtkI2S0dl1_dev)
		return -ENOMEM;

	ret = platform_device_add(soc_mtkI2S0dl1_dev);
	if (ret != 0) {
		platform_device_put(soc_mtkI2S0dl1_dev);
		return ret;
	}
#endif

	ret = platform_driver_register(&mtk_I2S0dl1_driver);
	return ret;

}
module_init(mtk_I2S0dl1_soc_platform_init);

static void __exit mtk_I2S0dl1_soc_platform_exit(void)
{
	pr_debug("%s\n", __func__);

	platform_driver_unregister(&mtk_I2S0dl1_driver);
}
module_exit(mtk_I2S0dl1_soc_platform_exit);

MODULE_DESCRIPTION("AFE PCM module platform driver");
MODULE_LICENSE("GPL");
