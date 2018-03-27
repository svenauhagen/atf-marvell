/*
 * Copyright (C) 2018 Marvell International Ltd.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 * https://spdx.org/licenses
 */

#include <debug.h>
#include <runtime_svc.h>
#include <smcc.h>
#include "comphy/phy-comphy-cp110.h"

/* #define DEBUG_COMPHY */
#ifdef DEBUG_COMPHY
#define debug(format...) NOTICE(format)
#else
#define debug(format, arg...)
#endif

#define MV_SIP_COMPHY_POWER_ON	0x82000001
#define MV_SIP_COMPHY_POWER_OFF	0x82000002
#define MV_SIP_COMPHY_PLL_LOCK	0x82000003

#define MAX_LANE_NR		6
#define MVEBU_COMPHY_OFFSET	0x441000

/* This macro is used to identify COMPHY related calls from SMC function ID */
#define is_comphy_fid(fid)	\
	((fid) >= MV_SIP_COMPHY_POWER_ON && (fid) <= MV_SIP_COMPHY_PLL_LOCK)


uint64_t mrvl_sip_smc_handler(uint32_t smc_fid,
			      uint64_t x1,
			      uint64_t x2,
			      uint64_t x3,
			      uint64_t x4,
			      void *cookie,
			      void *handle,
			      uint64_t flags)
{
	u_register_t ret;

	debug("%s: got SMC (0x%x) x1 0x%lx, x2 0x%lx, x3 0x%lx\n",
						 __func__, smc_fid, x1, x2, x3);
	if (is_comphy_fid(smc_fid)) {

		if (!(x1 & MVEBU_COMPHY_OFFSET)) {
			ERROR("%s: Wrong smc (0x%x) address: %lx\n",  __func__, smc_fid, x1);
			SMC_RET1(handle, SMC_UNK);
		}

		if (x2 >= MAX_LANE_NR) {
			ERROR("%s: Wrong smc (0x%x) lane nr: %lx\n",  __func__, smc_fid, x2);
			SMC_RET1(handle, SMC_UNK);
		}
	}

	switch (smc_fid) {
	case MV_SIP_COMPHY_POWER_ON:
		/* x1:  comphy_base, x2: comphy_index, x3: comphy_mode */
		ret = mvebu_cp110_comphy_power_on(x1, x2, x3);
		SMC_RET1(handle, ret);
	case MV_SIP_COMPHY_POWER_OFF:
		/* x1:  comphy_base, x2: comphy_index */
		ret = mvebu_cp110_comphy_power_off(x1, x2);
		SMC_RET1(handle, ret);
	case MV_SIP_COMPHY_PLL_LOCK:
		/* x1:  comphy_base, x2: comphy_index */
		ret = mvebu_cp110_comphy_is_pll_locked(x1, x2);
		SMC_RET1(handle, ret);
	default:
		ERROR("%s: unhandled SMC (0x%x)\n", __func__, smc_fid);
		SMC_RET1(handle, SMC_UNK);
	}
}

/* Define a runtime service descriptor for fast SMC calls */
DECLARE_RT_SVC(
	marvell_sip_svc,
	OEN_SIP_START,
	OEN_SIP_END,
	SMC_TYPE_FAST,
	NULL,
	mrvl_sip_smc_handler
);