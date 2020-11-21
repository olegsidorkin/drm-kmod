/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#define SWSMU_CODE_LAYER_L2

#include <linux/firmware.h>
#include "amdgpu.h"
#include "amdgpu_smu.h"
#include "atomfirmware.h"
#include "amdgpu_atomfirmware.h"
#include "amdgpu_atombios.h"
#include "smu_v13_0.h"
#include "smu13_driver_if_aldebaran.h"
#include "soc15_common.h"
#include "atom.h"
#include "power_state.h"
#include "aldebaran_ppt.h"
#include "smu_v13_0_pptable.h"
#include "aldebaran_ppsmc.h"
#include "nbio/nbio_7_4_offset.h"
#include "nbio/nbio_7_4_sh_mask.h"
#include "thm/thm_11_0_2_offset.h"
#include "thm/thm_11_0_2_sh_mask.h"
#include "amdgpu_xgmi.h"
#include <linux/pci.h>
#include "amdgpu_ras.h"
#include "smu_cmn.h"
#include "mp/mp_13_0_2_offset.h"

/*
 * DO NOT use these for err/warn/info/debug messages.
 * Use dev_err, dev_warn, dev_info and dev_dbg instead.
 * They are more MGPU friendly.
 */
#undef pr_err
#undef pr_warn
#undef pr_info
#undef pr_debug

#define to_amdgpu_device(x) (container_of(x, struct amdgpu_device, pm.smu_i2c))

#define ALDEBARAN_FEA_MAP(smu_feature, aldebaran_feature) \
	[smu_feature] = {1, (aldebaran_feature)}

#define FEATURE_MASK(feature) (1ULL << feature)
#define SMC_DPM_FEATURE ( \
			  FEATURE_MASK(FEATURE_DATA_CALCULATIONS) | \
			  FEATURE_MASK(FEATURE_DPM_GFXCLK_BIT)	| \
			  FEATURE_MASK(FEATURE_DPM_UCLK_BIT)	| \
			  FEATURE_MASK(FEATURE_DPM_SOCCLK_BIT)	| \
			  FEATURE_MASK(FEATURE_DPM_FCLK_BIT)	| \
			  FEATURE_MASK(FEATURE_DPM_LCLK_BIT)	| \
			  FEATURE_MASK(FEATURE_DPM_XGMI_BIT)	| \
			  FEATURE_MASK(FEATURE_DPM_VCN_BIT))

/* possible frequency drift (1Mhz) */
#define EPSILON				1

#define smnPCIE_ESM_CTRL			0x111003D0

static const struct cmn2asic_msg_mapping aldebaran_message_map[SMU_MSG_MAX_COUNT] = {
	MSG_MAP(TestMessage,			     PPSMC_MSG_TestMessage,			0),
	MSG_MAP(GetSmuVersion,			     PPSMC_MSG_GetSmuVersion,			1),
	MSG_MAP(GetDriverIfVersion,		     PPSMC_MSG_GetDriverIfVersion,		1),
	MSG_MAP(EnableAllSmuFeatures,		     PPSMC_MSG_EnableAllSmuFeatures,		0),
	MSG_MAP(DisableAllSmuFeatures,		     PPSMC_MSG_DisableAllSmuFeatures,		0),
	MSG_MAP(GetEnabledSmuFeaturesLow,	     PPSMC_MSG_GetEnabledSmuFeaturesLow,	0),
	MSG_MAP(GetEnabledSmuFeaturesHigh,	     PPSMC_MSG_GetEnabledSmuFeaturesHigh,	0),
	MSG_MAP(SetDriverDramAddrHigh,		     PPSMC_MSG_SetDriverDramAddrHigh,		1),
	MSG_MAP(SetDriverDramAddrLow,		     PPSMC_MSG_SetDriverDramAddrLow,		1),
	MSG_MAP(SetToolsDramAddrHigh,		     PPSMC_MSG_SetToolsDramAddrHigh,		0),
	MSG_MAP(SetToolsDramAddrLow,		     PPSMC_MSG_SetToolsDramAddrLow,		0),
	MSG_MAP(TransferTableSmu2Dram,		     PPSMC_MSG_TransferTableSmu2Dram,		1),
	MSG_MAP(TransferTableDram2Smu,		     PPSMC_MSG_TransferTableDram2Smu,		0),
	MSG_MAP(UseDefaultPPTable,		     PPSMC_MSG_UseDefaultPPTable,		0),
	MSG_MAP(SetSystemVirtualDramAddrHigh,	     PPSMC_MSG_SetSystemVirtualDramAddrHigh,	0),
	MSG_MAP(SetSystemVirtualDramAddrLow,	     PPSMC_MSG_SetSystemVirtualDramAddrLow,	0),
	MSG_MAP(SetSoftMinByFreq,		     PPSMC_MSG_SetSoftMinByFreq,		0),
	MSG_MAP(SetSoftMaxByFreq,		     PPSMC_MSG_SetSoftMaxByFreq,		0),
	MSG_MAP(SetHardMinByFreq,		     PPSMC_MSG_SetHardMinByFreq,		0),
	MSG_MAP(SetHardMaxByFreq,		     PPSMC_MSG_SetHardMaxByFreq,		0),
	MSG_MAP(GetMinDpmFreq,			     PPSMC_MSG_GetMinDpmFreq,			0),
	MSG_MAP(GetMaxDpmFreq,			     PPSMC_MSG_GetMaxDpmFreq,			0),
	MSG_MAP(GetDpmFreqByIndex,		     PPSMC_MSG_GetDpmFreqByIndex,		1),
	MSG_MAP(SetWorkloadMask,		     PPSMC_MSG_SetWorkloadMask,			1),
	MSG_MAP(GetVoltageByDpm,		     PPSMC_MSG_GetVoltageByDpm,			0),
	MSG_MAP(GetVoltageByDpmOverdrive,	     PPSMC_MSG_GetVoltageByDpmOverdrive,	0),
	MSG_MAP(SetPptLimit,			     PPSMC_MSG_SetPptLimit,			0),
	MSG_MAP(GetPptLimit,			     PPSMC_MSG_GetPptLimit,			1),
	MSG_MAP(PrepareMp1ForUnload,		     PPSMC_MSG_PrepareMp1ForUnload,		0),
	MSG_MAP(PrepareMp1ForReset,		     PPSMC_MSG_PrepareMp1ForReset,		0),
	MSG_MAP(GfxDriverReset,			     PPSMC_MSG_GfxDriverReset,			0),
	MSG_MAP(SoftReset,			     PPSMC_MSG_SoftReset,			0),
	MSG_MAP(RunDcBtc,			     PPSMC_MSG_RunDcBtc,			0),
	MSG_MAP(DramLogSetDramAddrHigh,		     PPSMC_MSG_DramLogSetDramAddrHigh,		0),
	MSG_MAP(DramLogSetDramAddrLow,		     PPSMC_MSG_DramLogSetDramAddrLow,		0),
	MSG_MAP(DramLogSetDramSize,		     PPSMC_MSG_DramLogSetDramSize,		0),
	MSG_MAP(GetDebugData,			     PPSMC_MSG_GetDebugData,			0),
	MSG_MAP(WaflTest,			     PPSMC_MSG_WaflTest,			0),
	MSG_MAP(SetMemoryChannelEnable,		     PPSMC_MSG_SetMemoryChannelEnable,		0),
	MSG_MAP(SetNumBadHbmPagesRetired,	     PPSMC_MSG_SetNumBadHbmPagesRetired,	0),
	MSG_MAP(DFCstateControl,		     PPSMC_MSG_DFCstateControl,			0),
	MSG_MAP(GetGmiPwrDnHyst,		     PPSMC_MSG_GetGmiPwrDnHyst,			0),
	MSG_MAP(SetGmiPwrDnHyst,		     PPSMC_MSG_SetGmiPwrDnHyst,			0),
	MSG_MAP(GmiPwrDnControl,		     PPSMC_MSG_GmiPwrDnControl,			0),
	MSG_MAP(EnterGfxoff,			     PPSMC_MSG_EnterGfxoff,			0),
	MSG_MAP(ExitGfxoff,			     PPSMC_MSG_ExitGfxoff,			0),
	MSG_MAP(SetExecuteDMATest,		     PPSMC_MSG_SetExecuteDMATest,		0),
	MSG_MAP(EnableDeterminism,		     PPSMC_MSG_EnableDeterminism,		0),
	MSG_MAP(DisableDeterminism,		     PPSMC_MSG_DisableDeterminism,		0),
	MSG_MAP(SetUclkDpmMode,				 PPSMC_MSG_SetUclkDpmMode,		0),
};

static const struct cmn2asic_mapping aldebaran_clk_map[SMU_CLK_COUNT] = {
	CLK_MAP(GFXCLK, PPCLK_GFXCLK),
	CLK_MAP(SCLK,	PPCLK_GFXCLK),
	CLK_MAP(SOCCLK, PPCLK_SOCCLK),
	CLK_MAP(FCLK, PPCLK_FCLK),
	CLK_MAP(UCLK, PPCLK_UCLK),
	CLK_MAP(MCLK, PPCLK_UCLK),
	CLK_MAP(DCLK, PPCLK_DCLK),
	CLK_MAP(VCLK, PPCLK_VCLK),
	CLK_MAP(LCLK, 	PPCLK_LCLK),
};

static const struct cmn2asic_mapping aldebaran_feature_mask_map[SMU_FEATURE_COUNT] = {
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DPM_PREFETCHER_BIT, 		FEATURE_DATA_CALCULATIONS),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DPM_GFXCLK_BIT, 			FEATURE_DPM_GFXCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DPM_UCLK_BIT, 			FEATURE_DPM_UCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DPM_SOCCLK_BIT, 			FEATURE_DPM_SOCCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DPM_FCLK_BIT, 			FEATURE_DPM_FCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DPM_LCLK_BIT, 			FEATURE_DPM_LCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_XGMI_BIT, 				FEATURE_DPM_XGMI_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DS_GFXCLK_BIT, 			FEATURE_DS_GFXCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DS_SOCCLK_BIT, 			FEATURE_DS_SOCCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DS_LCLK_BIT, 				FEATURE_DS_LCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DS_FCLK_BIT, 				FEATURE_DS_FCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DS_UCLK_BIT,				FEATURE_DS_UCLK_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_GFX_SS_BIT, 				FEATURE_GFX_SS_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_VCN_PG_BIT, 				FEATURE_DPM_VCN_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_RSMU_SMN_CG_BIT, 			FEATURE_RSMU_SMN_CG_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_WAFL_CG_BIT, 				FEATURE_WAFL_CG_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_PPT_BIT, 					FEATURE_PPT_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_TDC_BIT, 					FEATURE_TDC_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_APCC_PLUS_BIT, 			FEATURE_APCC_PLUS_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_APCC_DFLL_BIT, 			FEATURE_APCC_DFLL_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_FUSE_CG_BIT, 				FEATURE_FUSE_CG_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_MP1_CG_BIT, 				FEATURE_MP1_CG_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_SMUIO_CG_BIT, 			FEATURE_SMUIO_CG_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_THM_CG_BIT, 				FEATURE_THM_CG_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_CLK_CG_BIT, 				FEATURE_CLK_CG_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_FW_CTF_BIT, 				FEATURE_FW_CTF_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_THERMAL_BIT, 				FEATURE_THERMAL_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_OUT_OF_BAND_MONITOR_BIT, 	FEATURE_OUT_OF_BAND_MONITOR_BIT),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_XGMI_PER_LINK_PWR_DWN_BIT,FEATURE_XGMI_PER_LINK_PWR_DWN),
	ALDEBARAN_FEA_MAP(SMU_FEATURE_DF_CSTATE_BIT, 			FEATURE_DF_CSTATE),
};

static const struct cmn2asic_mapping aldebaran_table_map[SMU_TABLE_COUNT] = {
	TAB_MAP(PPTABLE),
	TAB_MAP(AVFS_PSM_DEBUG),
	TAB_MAP(AVFS_FUSE_OVERRIDE),
	TAB_MAP(PMSTATUSLOG),
	TAB_MAP(SMU_METRICS),
	TAB_MAP(DRIVER_SMU_CONFIG),
	TAB_MAP(I2C_COMMANDS),
};

static int aldebaran_tables_init(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *tables = smu_table->tables;

	SMU_TABLE_INIT(tables, SMU_TABLE_PPTABLE, sizeof(PPTable_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	SMU_TABLE_INIT(tables, SMU_TABLE_PMSTATUSLOG, SMU13_TOOL_SIZE,
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	SMU_TABLE_INIT(tables, SMU_TABLE_SMU_METRICS, sizeof(SmuMetrics_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	SMU_TABLE_INIT(tables, SMU_TABLE_I2C_COMMANDS, sizeof(SwI2cRequest_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	smu_table->metrics_table = kzalloc(sizeof(SmuMetrics_t), GFP_KERNEL);
	if (!smu_table->metrics_table)
		return -ENOMEM;
	smu_table->metrics_time = 0;

	smu_table->gpu_metrics_table_size = sizeof(struct gpu_metrics_v1_0);
	smu_table->gpu_metrics_table = kzalloc(smu_table->gpu_metrics_table_size, GFP_KERNEL);
	if (!smu_table->gpu_metrics_table) {
		kfree(smu_table->metrics_table);
		return -ENOMEM;
	}

	return 0;
}

static int aldebaran_allocate_dpm_context(struct smu_context *smu)
{
	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;

	smu_dpm->dpm_context = kzalloc(sizeof(struct smu_13_0_dpm_context),
				       GFP_KERNEL);
	if (!smu_dpm->dpm_context)
		return -ENOMEM;
	smu_dpm->dpm_context_size = sizeof(struct smu_13_0_dpm_context);

	smu_dpm->dpm_current_power_state = kzalloc(sizeof(struct smu_power_state),
						   GFP_KERNEL);
	if (!smu_dpm->dpm_current_power_state)
		return -ENOMEM;

	smu_dpm->dpm_request_power_state = kzalloc(sizeof(struct smu_power_state),
						   GFP_KERNEL);
	if (!smu_dpm->dpm_request_power_state)
		return -ENOMEM;

	return 0;
}

static int aldebaran_init_smc_tables(struct smu_context *smu)
{
	int ret = 0;

	ret = aldebaran_tables_init(smu);
	if (ret)
		return ret;

	ret = aldebaran_allocate_dpm_context(smu);
	if (ret)
		return ret;

	return smu_v13_0_init_smc_tables(smu);
}

static int aldebaran_get_allowed_feature_mask(struct smu_context *smu,
					      uint32_t *feature_mask, uint32_t num)
{
	if (num > 2)
		return -EINVAL;

	/* pptable will handle the features to enable */
	memset(feature_mask, 0xFF, sizeof(uint32_t) * num);

	return 0;
}

static int aldebaran_set_default_dpm_table(struct smu_context *smu)
{
	struct smu_13_0_dpm_context *dpm_context = smu->smu_dpm.dpm_context;
	struct smu_13_0_dpm_table *dpm_table = NULL;
	PPTable_t *pptable = smu->smu_table.driver_pptable;
	int ret = 0;

	/* socclk dpm table setup */
	dpm_table = &dpm_context->dpm_tables.soc_table;
	if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_SOCCLK_BIT)) {
		ret = smu_v13_0_set_single_dpm_table(smu,
						     SMU_SOCCLK,
						     dpm_table);
		if (ret)
			return ret;
	} else {
		dpm_table->count = 1;
		dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.socclk / 100;
		dpm_table->dpm_levels[0].enabled = true;
		dpm_table->min = dpm_table->dpm_levels[0].value;
		dpm_table->max = dpm_table->dpm_levels[0].value;
	}

	/* gfxclk dpm table setup */
	dpm_table = &dpm_context->dpm_tables.gfx_table;
	if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_GFXCLK_BIT)) {
		/* in the case of gfxclk, only fine-grained dpm is honored */
		dpm_table->count = 2;
		dpm_table->dpm_levels[0].value = pptable->GfxclkFmin;
		dpm_table->dpm_levels[0].enabled = true;
		dpm_table->dpm_levels[1].value = pptable->GfxclkFmax;
		dpm_table->dpm_levels[1].enabled = true;
		dpm_table->min = dpm_table->dpm_levels[0].value;
		dpm_table->max = dpm_table->dpm_levels[1].value;
	} else {
		dpm_table->count = 1;
		dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.gfxclk / 100;
		dpm_table->dpm_levels[0].enabled = true;
		dpm_table->min = dpm_table->dpm_levels[0].value;
		dpm_table->max = dpm_table->dpm_levels[0].value;
	}

	/* memclk dpm table setup */
	dpm_table = &dpm_context->dpm_tables.uclk_table;
	if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_UCLK_BIT)) {
		ret = smu_v13_0_set_single_dpm_table(smu,
						     SMU_UCLK,
						     dpm_table);
		if (ret)
			return ret;
	} else {
		dpm_table->count = 1;
		dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.uclk / 100;
		dpm_table->dpm_levels[0].enabled = true;
		dpm_table->min = dpm_table->dpm_levels[0].value;
		dpm_table->max = dpm_table->dpm_levels[0].value;
	}

	/* fclk dpm table setup */
	dpm_table = &dpm_context->dpm_tables.fclk_table;
	if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_FCLK_BIT)) {
		ret = smu_v13_0_set_single_dpm_table(smu,
						     SMU_FCLK,
						     dpm_table);
		if (ret)
			return ret;
	} else {
		dpm_table->count = 1;
		dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.fclk / 100;
		dpm_table->dpm_levels[0].enabled = true;
		dpm_table->min = dpm_table->dpm_levels[0].value;
		dpm_table->max = dpm_table->dpm_levels[0].value;
	}

	return 0;
}

static int aldebaran_check_powerplay_table(struct smu_context *smu)
{
	struct smu_table_context *table_context = &smu->smu_table;
	struct smu_13_0_powerplay_table *powerplay_table =
		table_context->power_play_table;
	struct smu_baco_context *smu_baco = &smu->smu_baco;

	mutex_lock(&smu_baco->mutex);
	if (powerplay_table->platform_caps & SMU_13_0_PP_PLATFORM_CAP_BACO ||
	    powerplay_table->platform_caps & SMU_13_0_PP_PLATFORM_CAP_MACO)
		smu_baco->platform_support = true;
	mutex_unlock(&smu_baco->mutex);

	table_context->thermal_controller_type =
		powerplay_table->thermal_controller_type;

	return 0;
}

static int aldebaran_store_powerplay_table(struct smu_context *smu)
{
	struct smu_table_context *table_context = &smu->smu_table;
	struct smu_13_0_powerplay_table *powerplay_table =
		table_context->power_play_table;
	memcpy(table_context->driver_pptable, &powerplay_table->smc_pptable,
	       sizeof(PPTable_t));

	return 0;
}

static int aldebaran_append_powerplay_table(struct smu_context *smu)
{
	return 0;
}

static int aldebaran_setup_pptable(struct smu_context *smu)
{
	int ret = 0;

	ret = smu_v13_0_setup_pptable(smu);
	if (ret)
		return ret;

	ret = aldebaran_store_powerplay_table(smu);
	if (ret)
		return ret;

	ret = aldebaran_append_powerplay_table(smu);
	if (ret)
		return ret;

	ret = aldebaran_check_powerplay_table(smu);
	if (ret)
		return ret;

	return ret;
}

static int aldebaran_run_btc(struct smu_context *smu)
{
	/* int ret = 0; */

	/* ret = smu_cmn_send_smc_msg(smu, SMU_MSG_RunAfllBtc, NULL); */
	/* if (ret) { */
	/* dev_err(smu->adev->dev, "RunAfllBtc failed!\n"); */
	/* return ret; */
	/* } */

	/* return smu_cmn_send_smc_msg(smu, SMU_MSG_RunDcBtc, NULL); */
	return 0;
}

static int aldebaran_populate_umd_state_clk(struct smu_context *smu)
{
	struct smu_13_0_dpm_context *dpm_context =
		smu->smu_dpm.dpm_context;
	struct smu_13_0_dpm_table *gfx_table =
		&dpm_context->dpm_tables.gfx_table;
	struct smu_13_0_dpm_table *mem_table =
		&dpm_context->dpm_tables.uclk_table;
	struct smu_13_0_dpm_table *soc_table =
		&dpm_context->dpm_tables.soc_table;
	struct smu_umd_pstate_table *pstate_table =
		&smu->pstate_table;

	pstate_table->gfxclk_pstate.min = gfx_table->min;
	pstate_table->gfxclk_pstate.peak = gfx_table->max;

	pstate_table->uclk_pstate.min = mem_table->min;
	pstate_table->uclk_pstate.peak = mem_table->max;

	pstate_table->socclk_pstate.min = soc_table->min;
	pstate_table->socclk_pstate.peak = soc_table->max;

	if (gfx_table->count > ALDEBARAN_UMD_PSTATE_GFXCLK_LEVEL &&
	    mem_table->count > ALDEBARAN_UMD_PSTATE_MCLK_LEVEL &&
	    soc_table->count > ALDEBARAN_UMD_PSTATE_SOCCLK_LEVEL) {
		pstate_table->gfxclk_pstate.standard =
			gfx_table->dpm_levels[ALDEBARAN_UMD_PSTATE_GFXCLK_LEVEL].value;
		pstate_table->uclk_pstate.standard =
			mem_table->dpm_levels[ALDEBARAN_UMD_PSTATE_MCLK_LEVEL].value;
		pstate_table->socclk_pstate.standard =
			soc_table->dpm_levels[ALDEBARAN_UMD_PSTATE_SOCCLK_LEVEL].value;
	} else {
		pstate_table->gfxclk_pstate.standard =
			pstate_table->gfxclk_pstate.min;
		pstate_table->uclk_pstate.standard =
			pstate_table->uclk_pstate.min;
		pstate_table->socclk_pstate.standard =
			pstate_table->socclk_pstate.min;
	}

	return 0;
}

static int aldebaran_get_clk_table(struct smu_context *smu,
				   struct pp_clock_levels_with_latency *clocks,
				   struct smu_13_0_dpm_table *dpm_table)
{
	int i, count;

	count = (dpm_table->count > MAX_NUM_CLOCKS) ? MAX_NUM_CLOCKS : dpm_table->count;
	clocks->num_levels = count;

	for (i = 0; i < count; i++) {
		clocks->data[i].clocks_in_khz =
			dpm_table->dpm_levels[i].value * 1000;
		clocks->data[i].latency_in_us = 0;
	}

	return 0;
}

static int aldebaran_freqs_in_same_level(int32_t frequency1,
					 int32_t frequency2)
{
	return (abs(frequency1 - frequency2) <= EPSILON);
}

static int aldebaran_get_smu_metrics_data(struct smu_context *smu,
					  MetricsMember_t member,
					  uint32_t *value)
{
	struct smu_table_context *smu_table= &smu->smu_table;
	SmuMetrics_t *metrics = (SmuMetrics_t *)smu_table->metrics_table;
	int ret = 0;

	mutex_lock(&smu->metrics_lock);

	ret = smu_cmn_get_metrics_table_locked(smu,
					       NULL,
					       false);
	if (ret) {
		mutex_unlock(&smu->metrics_lock);
		return ret;
	}

	switch (member) {
	case METRICS_CURR_GFXCLK:
		*value = metrics->CurrClock[PPCLK_GFXCLK];
		break;
	case METRICS_CURR_SOCCLK:
		*value = metrics->CurrClock[PPCLK_SOCCLK];
		break;
	case METRICS_CURR_UCLK:
		*value = metrics->CurrClock[PPCLK_UCLK];
		break;
	case METRICS_CURR_VCLK:
		*value = metrics->CurrClock[PPCLK_VCLK];
		break;
	case METRICS_CURR_DCLK:
		*value = metrics->CurrClock[PPCLK_DCLK];
		break;
	case METRICS_CURR_FCLK:
		*value = metrics->CurrClock[PPCLK_FCLK];
		break;
	case METRICS_AVERAGE_GFXCLK:
		*value = metrics->AverageGfxclkFrequency;
		break;
	case METRICS_AVERAGE_SOCCLK:
		*value = metrics->AverageSocclkFrequency;
		break;
	case METRICS_AVERAGE_UCLK:
		*value = metrics->AverageUclkFrequency;
		break;
	case METRICS_AVERAGE_GFXACTIVITY:
		*value = metrics->AverageGfxActivity;
		break;
	case METRICS_AVERAGE_MEMACTIVITY:
		*value = metrics->AverageUclkActivity;
		break;
	case METRICS_AVERAGE_SOCKETPOWER:
		*value = metrics->AverageSocketPower << 8;
		break;
	case METRICS_TEMPERATURE_EDGE:
		*value = metrics->TemperatureEdge *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case METRICS_TEMPERATURE_HOTSPOT:
		*value = metrics->TemperatureHotspot *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case METRICS_TEMPERATURE_MEM:
		*value = metrics->TemperatureHBM *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case METRICS_TEMPERATURE_VRGFX:
		*value = metrics->TemperatureVrGfx *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case METRICS_TEMPERATURE_VRSOC:
		*value = metrics->TemperatureVrSoc *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case METRICS_TEMPERATURE_VRMEM:
		*value = metrics->TemperatureVrMem *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case METRICS_THROTTLER_STATUS:
		*value = metrics->ThrottlerStatus;
		break;
	default:
		*value = UINT_MAX;
		break;
	}

	mutex_unlock(&smu->metrics_lock);

	return ret;
}

static int aldebaran_get_current_clk_freq_by_table(struct smu_context *smu,
						   enum smu_clk_type clk_type,
						   uint32_t *value)
{
	MetricsMember_t member_type;
	int clk_id = 0;

	if (!value)
		return -EINVAL;

	clk_id = smu_cmn_to_asic_specific_index(smu,
						CMN2ASIC_MAPPING_CLK,
						clk_type);
	if (clk_id < 0)
		return -EINVAL;

	switch (clk_id) {
	case PPCLK_GFXCLK:
		/*
		 * CurrClock[clk_id] can provide accurate
		 *   output only when the dpm feature is enabled.
		 * We can use Average_* for dpm disabled case.
		 *   But this is available for gfxclk/uclk/socclk/vclk/dclk.
		 */
		if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_GFXCLK_BIT))
			member_type = METRICS_CURR_GFXCLK;
		else
			member_type = METRICS_AVERAGE_GFXCLK;
		break;
	case PPCLK_UCLK:
		if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_UCLK_BIT))
			member_type = METRICS_CURR_UCLK;
		else
			member_type = METRICS_AVERAGE_UCLK;
		break;
	case PPCLK_SOCCLK:
		if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_SOCCLK_BIT))
			member_type = METRICS_CURR_SOCCLK;
		else
			member_type = METRICS_AVERAGE_SOCCLK;
		break;
	case PPCLK_VCLK:
		if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_VCN_PG_BIT))
			member_type = METRICS_CURR_VCLK;
		else
			member_type = METRICS_AVERAGE_VCLK;
		break;
	case PPCLK_DCLK:
		if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_VCN_PG_BIT))
			member_type = METRICS_CURR_DCLK;
		else
			member_type = METRICS_AVERAGE_DCLK;
		break;
	case PPCLK_FCLK:
		member_type = METRICS_CURR_FCLK;
		break;
	default:
		return -EINVAL;
	}

	return aldebaran_get_smu_metrics_data(smu,
					      member_type,
					      value);
}

static int aldebaran_print_clk_levels(struct smu_context *smu,
				      enum smu_clk_type type, char *buf)
{
	int i, now, size = 0;
	int ret = 0;
	struct pp_clock_levels_with_latency clocks;
	struct smu_13_0_dpm_table *single_dpm_table;
	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;
	struct smu_13_0_dpm_context *dpm_context = NULL;
	uint32_t display_levels;
	uint32_t freq_values[3] = {0};

	if (amdgpu_ras_intr_triggered())
		return snprintf(buf, PAGE_SIZE, "unavailable\n");

	dpm_context = smu_dpm->dpm_context;

	switch (type) {
	case SMU_SCLK:
		ret = aldebaran_get_current_clk_freq_by_table(smu, SMU_GFXCLK, &now);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get current gfx clk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_context->dpm_tables.gfx_table);
		ret = aldebaran_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get gfx clk levels Failed!");
			return ret;
		}

		display_levels = clocks.num_levels;

		/* fine-grained dpm has only 2 levels */
		if (now > single_dpm_table->dpm_levels[0].value &&
				now < single_dpm_table->dpm_levels[1].value) {
			display_levels = clocks.num_levels + 1;
			freq_values[0] = single_dpm_table->dpm_levels[0].value;
			freq_values[2] = single_dpm_table->dpm_levels[1].value;
			freq_values[1] = now;
		}

		/*
		 * For DPM disabled case, there will be only one clock level.
		 * And it's safe to assume that is always the current clock.
		 */
		if (display_levels == clocks.num_levels) {
			for (i = 0; i < clocks.num_levels; i++)
				size += sprintf(buf + size, "%d: %uMhz %s\n", i,
						clocks.data[i].clocks_in_khz / 1000,
						(clocks.num_levels == 1) ? "*" :
						(aldebaran_freqs_in_same_level(
								       clocks.data[i].clocks_in_khz / 1000,
								       now) ? "*" : ""));
		} else {
			for (i = 0; i < display_levels; i++)
				size += sprintf(buf + size, "%d: %uMhz %s\n", i,
						freq_values[i], i == 1 ? "*" : "");
		}

		break;

	case SMU_MCLK:
		ret = aldebaran_get_current_clk_freq_by_table(smu, SMU_UCLK, &now);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get current mclk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_context->dpm_tables.uclk_table);
		ret = aldebaran_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get memory clk levels Failed!");
			return ret;
		}

		for (i = 0; i < clocks.num_levels; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n",
					i, clocks.data[i].clocks_in_khz / 1000,
					(clocks.num_levels == 1) ? "*" :
					(aldebaran_freqs_in_same_level(
								       clocks.data[i].clocks_in_khz / 1000,
								       now) ? "*" : ""));
		break;

	case SMU_SOCCLK:
		ret = aldebaran_get_current_clk_freq_by_table(smu, SMU_SOCCLK, &now);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get current socclk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_context->dpm_tables.soc_table);
		ret = aldebaran_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get socclk levels Failed!");
			return ret;
		}

		for (i = 0; i < clocks.num_levels; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n",
					i, clocks.data[i].clocks_in_khz / 1000,
					(clocks.num_levels == 1) ? "*" :
					(aldebaran_freqs_in_same_level(
								       clocks.data[i].clocks_in_khz / 1000,
								       now) ? "*" : ""));
		break;

	case SMU_FCLK:
		ret = aldebaran_get_current_clk_freq_by_table(smu, SMU_FCLK, &now);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get current fclk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_context->dpm_tables.fclk_table);
		ret = aldebaran_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			dev_err(smu->adev->dev, "Attempt to get fclk levels Failed!");
			return ret;
		}

		for (i = 0; i < single_dpm_table->count; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n",
					i, single_dpm_table->dpm_levels[i].value,
					(clocks.num_levels == 1) ? "*" :
					(aldebaran_freqs_in_same_level(
								       clocks.data[i].clocks_in_khz / 1000,
								       now) ? "*" : ""));
		break;

	default:
		break;
	}

	return size;
}

static int aldebaran_upload_dpm_level(struct smu_context *smu,
				      bool max,
				      uint32_t feature_mask,
				      uint32_t level)
{
	struct smu_13_0_dpm_context *dpm_context =
		smu->smu_dpm.dpm_context;
	uint32_t freq;
	int ret = 0;

	if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_GFXCLK_BIT) &&
	    (feature_mask & FEATURE_MASK(FEATURE_DPM_GFXCLK_BIT))) {
		freq = dpm_context->dpm_tables.gfx_table.dpm_levels[level].value;
		ret = smu_cmn_send_smc_msg_with_param(smu,
						      (max ? SMU_MSG_SetSoftMaxByFreq : SMU_MSG_SetSoftMinByFreq),
						      (PPCLK_GFXCLK << 16) | (freq & 0xffff),
						      NULL);
		if (ret) {
			dev_err(smu->adev->dev, "Failed to set soft %s gfxclk !\n",
				max ? "max" : "min");
			return ret;
		}
	}

	if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_UCLK_BIT) &&
	    (feature_mask & FEATURE_MASK(FEATURE_DPM_UCLK_BIT))) {
		freq = dpm_context->dpm_tables.uclk_table.dpm_levels[level].value;
		ret = smu_cmn_send_smc_msg_with_param(smu,
						      (max ? SMU_MSG_SetSoftMaxByFreq : SMU_MSG_SetSoftMinByFreq),
						      (PPCLK_UCLK << 16) | (freq & 0xffff),
						      NULL);
		if (ret) {
			dev_err(smu->adev->dev, "Failed to set soft %s memclk !\n",
				max ? "max" : "min");
			return ret;
		}
	}

	if (smu_cmn_feature_is_enabled(smu, SMU_FEATURE_DPM_SOCCLK_BIT) &&
	    (feature_mask & FEATURE_MASK(FEATURE_DPM_SOCCLK_BIT))) {
		freq = dpm_context->dpm_tables.soc_table.dpm_levels[level].value;
		ret = smu_cmn_send_smc_msg_with_param(smu,
						      (max ? SMU_MSG_SetSoftMaxByFreq : SMU_MSG_SetSoftMinByFreq),
						      (PPCLK_SOCCLK << 16) | (freq & 0xffff),
						      NULL);
		if (ret) {
			dev_err(smu->adev->dev, "Failed to set soft %s socclk !\n",
				max ? "max" : "min");
			return ret;
		}
	}

	return ret;
}

static int aldebaran_force_clk_levels(struct smu_context *smu,
				      enum smu_clk_type type, uint32_t mask)
{
	struct smu_13_0_dpm_context *dpm_context = smu->smu_dpm.dpm_context;
	struct smu_13_0_dpm_table *single_dpm_table = NULL;
	uint32_t soft_min_level, soft_max_level;
	int ret = 0;

	soft_min_level = mask ? (ffs(mask) - 1) : 0;
	soft_max_level = mask ? (fls(mask) - 1) : 0;

	switch (type) {
	case SMU_SCLK:
		single_dpm_table = &(dpm_context->dpm_tables.gfx_table);
		if (soft_max_level >= single_dpm_table->count) {
			dev_err(smu->adev->dev, "Clock level specified %d is over max allowed %d\n",
				soft_max_level, single_dpm_table->count - 1);
			ret = -EINVAL;
			break;
		}

		ret = aldebaran_upload_dpm_level(smu,
						 false,
						 FEATURE_MASK(FEATURE_DPM_GFXCLK_BIT),
						 soft_min_level);
		if (ret) {
			dev_err(smu->adev->dev, "Failed to upload boot level to lowest!\n");
			break;
		}

		ret = aldebaran_upload_dpm_level(smu,
						 true,
						 FEATURE_MASK(FEATURE_DPM_GFXCLK_BIT),
						 soft_max_level);
		if (ret)
			dev_err(smu->adev->dev, "Failed to upload dpm max level to highest!\n");

		break;

	case SMU_MCLK:
	case SMU_SOCCLK:
	case SMU_FCLK:
		/*
		 * Should not arrive here since aldebaran does not
		 * support mclk/socclk/fclk softmin/softmax settings
		 */
		ret = -EINVAL;
		break;

	default:
		break;
	}

	return ret;
}

static int aldebaran_get_thermal_temperature_range(struct smu_context *smu,
						   struct smu_temperature_range *range)
{
	struct smu_table_context *table_context = &smu->smu_table;
	struct smu_13_0_powerplay_table *powerplay_table =
		table_context->power_play_table;
	PPTable_t *pptable = smu->smu_table.driver_pptable;

	if (!range)
		return -EINVAL;

	memcpy(range, &smu13_thermal_policy[0], sizeof(struct smu_temperature_range));

	range->hotspot_crit_max = pptable->ThotspotLimit *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->hotspot_emergency_max = (pptable->ThotspotLimit + CTF_OFFSET_HOTSPOT) *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->mem_crit_max = pptable->TmemLimit *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->mem_emergency_max = (pptable->TmemLimit + CTF_OFFSET_MEM)*
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->software_shutdown_temp = powerplay_table->software_shutdown_temp;

	return 0;
}

static int aldebaran_get_current_activity_percent(struct smu_context *smu,
						  enum amd_pp_sensors sensor,
						  uint32_t *value)
{
	int ret = 0;

	if (!value)
		return -EINVAL;

	switch (sensor) {
	case AMDGPU_PP_SENSOR_GPU_LOAD:
		ret = aldebaran_get_smu_metrics_data(smu,
						     METRICS_AVERAGE_GFXACTIVITY,
						     value);
		break;
	case AMDGPU_PP_SENSOR_MEM_LOAD:
		ret = aldebaran_get_smu_metrics_data(smu,
						     METRICS_AVERAGE_MEMACTIVITY,
						     value);
		break;
	default:
		dev_err(smu->adev->dev, "Invalid sensor for retrieving clock activity\n");
		return -EINVAL;
	}

	return ret;
}

static int aldebaran_get_gpu_power(struct smu_context *smu, uint32_t *value)
{
	if (!value)
		return -EINVAL;

	return aldebaran_get_smu_metrics_data(smu,
					      METRICS_AVERAGE_SOCKETPOWER,
					      value);
}

static int aldebaran_thermal_get_temperature(struct smu_context *smu,
					     enum amd_pp_sensors sensor,
					     uint32_t *value)
{
	int ret = 0;

	if (!value)
		return -EINVAL;

	switch (sensor) {
	case AMDGPU_PP_SENSOR_HOTSPOT_TEMP:
		ret = aldebaran_get_smu_metrics_data(smu,
						     METRICS_TEMPERATURE_HOTSPOT,
						     value);
		break;
	case AMDGPU_PP_SENSOR_EDGE_TEMP:
		ret = aldebaran_get_smu_metrics_data(smu,
						     METRICS_TEMPERATURE_EDGE,
						     value);
		break;
	case AMDGPU_PP_SENSOR_MEM_TEMP:
		ret = aldebaran_get_smu_metrics_data(smu,
						     METRICS_TEMPERATURE_MEM,
						     value);
		break;
	default:
		dev_err(smu->adev->dev, "Invalid sensor for retrieving temp\n");
		return -EINVAL;
	}

	return ret;
}

static int aldebaran_read_sensor(struct smu_context *smu,
				 enum amd_pp_sensors sensor,
				 void *data, uint32_t *size)
{
	int ret = 0;

	if (amdgpu_ras_intr_triggered())
		return 0;

	if (!data || !size)
		return -EINVAL;

	mutex_lock(&smu->sensor_lock);
	switch (sensor) {
	case AMDGPU_PP_SENSOR_MEM_LOAD:
	case AMDGPU_PP_SENSOR_GPU_LOAD:
		ret = aldebaran_get_current_activity_percent(smu,
							     sensor,
							     (uint32_t *)data);
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_GPU_POWER:
		ret = aldebaran_get_gpu_power(smu, (uint32_t *)data);
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_HOTSPOT_TEMP:
	case AMDGPU_PP_SENSOR_EDGE_TEMP:
	case AMDGPU_PP_SENSOR_MEM_TEMP:
		ret = aldebaran_thermal_get_temperature(smu, sensor,
							(uint32_t *)data);
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_GFX_MCLK:
		ret = aldebaran_get_current_clk_freq_by_table(smu, SMU_UCLK, (uint32_t *)data);
		/* the output clock frequency in 10K unit */
		*(uint32_t *)data *= 100;
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_GFX_SCLK:
		ret = aldebaran_get_current_clk_freq_by_table(smu, SMU_GFXCLK, (uint32_t *)data);
		*(uint32_t *)data *= 100;
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_VDDGFX:
		ret = smu_v13_0_get_gfx_vdd(smu, (uint32_t *)data);
		*size = 4;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&smu->sensor_lock);

	return ret;
}

static int aldebaran_get_power_limit(struct smu_context *smu)
{
	struct smu_13_0_powerplay_table *powerplay_table =
		(struct smu_13_0_powerplay_table *)smu->smu_table.power_play_table;
	PPTable_t *pptable = smu->smu_table.driver_pptable;
	uint32_t power_limit, od_percent;

	if (smu_v13_0_get_current_power_limit(smu, &power_limit)) {
		/* the last hope to figure out the ppt limit */
		if (!pptable) {
			dev_err(smu->adev->dev, "Cannot get PPT limit due to pptable missing!");
			return -EINVAL;
		}
	}
	smu->current_power_limit = power_limit;

	if (smu->od_enabled) {
		od_percent = le32_to_cpu(powerplay_table->overdrive_table.max[SMU_13_0_ODSETTING_POWERPERCENTAGE]);


		dev_dbg(smu->adev->dev, "ODSETTING_POWERPERCENTAGE: %d (default: %d)\n", od_percent, power_limit);

		power_limit *= (100 + od_percent);
		power_limit /= 100;
	}
	smu->max_power_limit = power_limit;

	return 0;
}

static int aldebaran_set_performance_level(struct smu_context *smu,
					   enum amd_dpm_forced_level level)
{
	switch (level) {
	case AMD_DPM_FORCED_LEVEL_HIGH:
	case AMD_DPM_FORCED_LEVEL_LOW:
	case AMD_DPM_FORCED_LEVEL_PROFILE_STANDARD:
	case AMD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK:
	case AMD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK:
	case AMD_DPM_FORCED_LEVEL_PROFILE_PEAK:
	default:
		break;
	}

	return smu_v13_0_set_performance_level(smu, level);
}

static bool aldebaran_is_dpm_running(struct smu_context *smu)
{
	int ret = 0;
	uint32_t feature_mask[2];
	unsigned long feature_enabled;
	ret = smu_cmn_get_enabled_mask(smu, feature_mask, 2);
	feature_enabled = (unsigned long)((uint64_t)feature_mask[0] |
					  ((uint64_t)feature_mask[1] << 32));
	return !!(feature_enabled & SMC_DPM_FEATURE);
}

static void aldebaran_get_unique_id(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	uint32_t top32 = 0, bottom32 = 0;
	uint64_t id;

	/* Get the SN to turn into a Unique ID */
	smu_cmn_send_smc_msg(smu, SMU_MSG_ReadSerialNumTop32, &top32);
	smu_cmn_send_smc_msg(smu, SMU_MSG_ReadSerialNumBottom32, &bottom32);

	id = ((uint64_t)bottom32 << 32) | top32;
	adev->unique_id = id;
	/* For aldebaran-and-later, unique_id == serial_number, so convert it to a
	 * 16-digit HEX string for convenience and backwards-compatibility
	 */
	sprintf(adev->serial, "%llx", id);
}

static bool aldebaran_is_baco_supported(struct smu_context *smu)
{
	/* aldebaran is not support baco */

	return false;
}

static int aldebaran_set_df_cstate(struct smu_context *smu,
				   enum pp_df_cstate state)
{
	return smu_cmn_send_smc_msg_with_param(smu, SMU_MSG_DFCstateControl, state, NULL);
}

static int aldebaran_allow_xgmi_power_down(struct smu_context *smu, bool en)
{
	return smu_cmn_send_smc_msg_with_param(smu,
					       SMU_MSG_GmiPwrDnControl,
					       en ? 1 : 0,
					       NULL);
}

static const struct throttling_logging_label {
	uint32_t feature_mask;
	const char *label;
} logging_label[] = {
	{(1U << THROTTLER_TEMP_MEM_BIT), "HBM"},
	{(1U << THROTTLER_TEMP_VR_GFX_BIT), "VR of GFX rail"},
	{(1U << THROTTLER_TEMP_VR_MEM_BIT), "VR of HBM rail"},
	{(1U << THROTTLER_TEMP_VR_SOC_BIT), "VR of SOC rail"},
};
static void aldebaran_log_thermal_throttling_event(struct smu_context *smu)
{
	int ret;
	int throttler_idx, throtting_events = 0, buf_idx = 0;
	struct amdgpu_device *adev = smu->adev;
	uint32_t throttler_status;
	char log_buf[256];

	ret = aldebaran_get_smu_metrics_data(smu,
					     METRICS_THROTTLER_STATUS,
					     &throttler_status);
	if (ret)
		return;

	memset(log_buf, 0, sizeof(log_buf));
	for (throttler_idx = 0; throttler_idx < ARRAY_SIZE(logging_label);
	     throttler_idx++) {
		if (throttler_status & logging_label[throttler_idx].feature_mask) {
			throtting_events++;
			buf_idx += snprintf(log_buf + buf_idx,
					    sizeof(log_buf) - buf_idx,
					    "%s%s",
					    throtting_events > 1 ? " and " : "",
					    logging_label[throttler_idx].label);
			if (buf_idx >= sizeof(log_buf)) {
				dev_err(adev->dev, "buffer overflow!\n");
				log_buf[sizeof(log_buf) - 1] = '\0';
				break;
			}
		}
	}

	dev_warn(adev->dev, "WARN: GPU thermal throttling temperature reached, expect performance decrease. %s.\n",
		 log_buf);
	kgd2kfd_smi_event_throttle(smu->adev->kfd.dev, throttler_status);
}

static int aldebaran_get_current_pcie_link_speed(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	uint32_t esm_ctrl;

	/* TODO: confirm this on real target */
	esm_ctrl = RREG32_PCIE(smnPCIE_ESM_CTRL);
	if ((esm_ctrl >> 15) & 0x1FFFF)
		return (((esm_ctrl >> 8) & 0x3F) + 128);

	return smu_v13_0_get_current_pcie_link_speed(smu);
}

static ssize_t aldebaran_get_gpu_metrics(struct smu_context *smu,
					 void **table)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct gpu_metrics_v1_0 *gpu_metrics =
		(struct gpu_metrics_v1_0 *)smu_table->gpu_metrics_table;
	SmuMetrics_t metrics;
	int ret = 0;

	ret = smu_cmn_get_metrics_table(smu,
					&metrics,
					true);
	if (ret)
		return ret;

	smu_v13_0_init_gpu_metrics_v1_0(gpu_metrics);

	gpu_metrics->temperature_edge = metrics.TemperatureEdge;
	gpu_metrics->temperature_hotspot = metrics.TemperatureHotspot;
	gpu_metrics->temperature_mem = metrics.TemperatureHBM;
	gpu_metrics->temperature_vrgfx = metrics.TemperatureVrGfx;
	gpu_metrics->temperature_vrsoc = metrics.TemperatureVrSoc;
	gpu_metrics->temperature_vrmem = metrics.TemperatureVrMem;

	gpu_metrics->average_gfx_activity = metrics.AverageGfxActivity;
	gpu_metrics->average_umc_activity = metrics.AverageUclkActivity;

	gpu_metrics->average_socket_power = metrics.AverageSocketPower;

	gpu_metrics->average_gfxclk_frequency = metrics.AverageGfxclkFrequency;
	gpu_metrics->average_socclk_frequency = metrics.AverageSocclkFrequency;
	gpu_metrics->average_uclk_frequency = metrics.AverageUclkFrequency;

	gpu_metrics->current_gfxclk = metrics.CurrClock[PPCLK_GFXCLK];
	gpu_metrics->current_socclk = metrics.CurrClock[PPCLK_SOCCLK];
	gpu_metrics->current_uclk = metrics.CurrClock[PPCLK_UCLK];
	gpu_metrics->current_vclk0 = metrics.CurrClock[PPCLK_VCLK];
	gpu_metrics->current_dclk0 = metrics.CurrClock[PPCLK_DCLK];

	gpu_metrics->throttle_status = metrics.ThrottlerStatus;

	gpu_metrics->pcie_link_width =
		smu_v13_0_get_current_pcie_link_width(smu);
	gpu_metrics->pcie_link_speed =
		aldebaran_get_current_pcie_link_speed(smu);

	*table = (void *)gpu_metrics;

	return sizeof(struct gpu_metrics_v1_0);
}

static bool aldebaran_is_mode1_reset_supported(struct smu_context *smu)
{
#if 0
	struct amdgpu_device *adev = smu->adev;
	u32 smu_version;
	uint32_t val;
	/**
	 * PM FW version support mode1 reset from 68.07
	 */
	smu_cmn_get_smc_version(smu, NULL, &smu_version);
	if ((smu_version < 0x00440700))
		return false;
	/**
	 * mode1 reset relies on PSP, so we should check if
	 * PSP is alive.
	 */
	val = RREG32_SOC15(MP0, 0, regMP0_SMN_C2PMSG_81);

	return val != 0x0;
#endif
	return true;
}

static bool aldebaran_is_mode2_reset_supported(struct smu_context *smu)
{
	return true;
}

static const struct pptable_funcs aldebaran_ppt_funcs = {
	/* init dpm */
	.get_allowed_feature_mask = aldebaran_get_allowed_feature_mask,
	/* btc */
	.run_btc = aldebaran_run_btc,
	/* dpm/clk tables */
	.set_default_dpm_table = aldebaran_set_default_dpm_table,
	.populate_umd_state_clk = aldebaran_populate_umd_state_clk,
	.get_thermal_temperature_range = aldebaran_get_thermal_temperature_range,
	.print_clk_levels = aldebaran_print_clk_levels,
	.force_clk_levels = aldebaran_force_clk_levels,
	.read_sensor = aldebaran_read_sensor,
	.set_performance_level = aldebaran_set_performance_level,
	.get_power_limit = aldebaran_get_power_limit,
	.is_dpm_running = aldebaran_is_dpm_running,
	.get_unique_id = aldebaran_get_unique_id,
	.init_microcode = smu_v13_0_init_microcode,
	.load_microcode = smu_v13_0_load_microcode,
	.fini_microcode = smu_v13_0_fini_microcode,
	.init_smc_tables = aldebaran_init_smc_tables,
	.fini_smc_tables = smu_v13_0_fini_smc_tables,
	.init_power = smu_v13_0_init_power,
	.fini_power = smu_v13_0_fini_power,
	.check_fw_status = smu_v13_0_check_fw_status,
	/* pptable related */
	.setup_pptable = aldebaran_setup_pptable,
	.get_vbios_bootup_values = smu_v13_0_get_vbios_bootup_values,
	.check_fw_version = smu_v13_0_check_fw_version,
	.write_pptable = smu_cmn_write_pptable,
	.set_driver_table_location = smu_v13_0_set_driver_table_location,
	.set_tool_table_location = smu_v13_0_set_tool_table_location,
	.notify_memory_pool_location = smu_v13_0_notify_memory_pool_location,
	.system_features_control = smu_v13_0_system_features_control,
	.send_smc_msg_with_param = smu_cmn_send_smc_msg_with_param,
	.send_smc_msg = smu_cmn_send_smc_msg,
	.get_enabled_mask = smu_cmn_get_enabled_mask,
	.feature_is_enabled = smu_cmn_feature_is_enabled,
	.disable_all_features_with_exception = smu_cmn_disable_all_features_with_exception,
	.set_power_limit = smu_v13_0_set_power_limit,
	.init_max_sustainable_clocks = smu_v13_0_init_max_sustainable_clocks,
	.enable_thermal_alert = smu_v13_0_enable_thermal_alert,
	.disable_thermal_alert = smu_v13_0_disable_thermal_alert,
	.set_xgmi_pstate = smu_v13_0_set_xgmi_pstate,
	.register_irq_handler = smu_v13_0_register_irq_handler,
	.set_azalia_d3_pme = smu_v13_0_set_azalia_d3_pme,
	.get_max_sustainable_clocks_by_dc = smu_v13_0_get_max_sustainable_clocks_by_dc,
	.baco_is_support= aldebaran_is_baco_supported,
	.get_dpm_ultimate_freq = smu_v13_0_get_dpm_ultimate_freq,
	.set_soft_freq_limited_range = smu_v13_0_set_soft_freq_limited_range,
	.set_df_cstate = aldebaran_set_df_cstate,
	.allow_xgmi_power_down = aldebaran_allow_xgmi_power_down,
	.log_thermal_throttling_event = aldebaran_log_thermal_throttling_event,
	.get_pp_feature_mask = smu_cmn_get_pp_feature_mask,
	.set_pp_feature_mask = smu_cmn_set_pp_feature_mask,
	.get_gpu_metrics = aldebaran_get_gpu_metrics,
	.mode1_reset_is_support = aldebaran_is_mode1_reset_supported,
	.mode2_reset_is_support = aldebaran_is_mode2_reset_supported,
	.mode1_reset = smu_v13_0_mode1_reset,
	.mode2_reset = smu_v13_0_mode2_reset,
};

void aldebaran_set_ppt_funcs(struct smu_context *smu)
{
	smu->ppt_funcs = &aldebaran_ppt_funcs;
	smu->message_map = aldebaran_message_map;
	smu->clock_map = aldebaran_clk_map;
	smu->feature_map = aldebaran_feature_mask_map;
	smu->table_map = aldebaran_table_map;
}