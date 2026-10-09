/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 * Adapted from Intel i915 guc_actions_pf_abi.h and iov_actions_mmio_abi.h.
 */
#ifndef _GUC_MMIO_RELAY_ABI_H_
#define _GUC_MMIO_RELAY_ABI_H_
#include <linux/bits.h>

/* CTB event: header, VFID, magic/opcode, three payload dwords. */
#define GUC_ACTION_GUC2PF_MMIO_RELAY_SERVICE	0x5006
#define GUC_ACTION_PF2GUC_MMIO_RELAY_SUCCESS	0x5007
#define GUC_ACTION_PF2GUC_MMIO_RELAY_FAILURE	0x5008
#define GUC_MMIO_RELAY_EVENT_LEN			6u
#define GUC_MMIO_RELAY_MAGIC			GENMASK(27, 24)
#define GUC_MMIO_RELAY_OPCODE			GENMASK(23, 16)
#define GUC_MMIO_RELAY_MBZ			(GENMASK(31, 28) | GENMASK(15, 0))

#define IOV_MMIO_HANDSHAKE			0x01
#define IOV_MMIO_UPDATE_GGTT			0x02
#define IOV_MMIO_GET_RUNTIME			0x10
#define IOV_MMIO_GGTT_OFFSET			GENMASK(31, 12)
#define IOV_MMIO_GGTT_MODE			GENMASK(11, 10)
#define IOV_MMIO_GGTT_COPIES			GENMASK(9, 0)
#define IOV_MMIO_GGTT_DUPLICATE			0
#define IOV_MMIO_GGTT_REPLICATE			1
#define IOV_MMIO_GGTT_DUPLICATE_LAST		2
#define IOV_MMIO_GGTT_REPLICATE_LAST		3

#endif
