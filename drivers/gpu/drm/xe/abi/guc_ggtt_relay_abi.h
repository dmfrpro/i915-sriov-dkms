/* SPDX-License-Identifier: MIT */
/* Intel i915 IOV UPDATE_GGTT32 ABI, shared with the Windows VF driver. */
#ifndef _GUC_GGTT_RELAY_ABI_H_
#define _GUC_GGTT_RELAY_ABI_H_

#include <linux/bits.h>

#include "guc_relay_communication_abi.h"

#define GUC_RELAY_ACTION_VF2PF_UPDATE_GGTT32	0x0102u
#define VF2PF_GGTT_REQUEST_MIN_LEN		4u
#define VF2PF_GGTT_REQUEST_MAX_PTES		((GUC_RELAY_MSG_MAX_LEN - 2u) / 2u)
#define VF2PF_GGTT_OFFSET			GENMASK(31, 12)
#define VF2PF_GGTT_MODE				GENMASK(11, 10)
#define VF2PF_GGTT_COPIES				GENMASK(9, 0)
#define VF2PF_GGTT_MAX_EXPANDED_PTES		(1023u + VF2PF_GGTT_REQUEST_MAX_PTES)

#endif
