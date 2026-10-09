/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2024 Intel Corporation
 */

#ifndef _XE_GT_SRIOV_PF_SERVICE_TYPES_H_
#define _XE_GT_SRIOV_PF_SERVICE_TYPES_H_

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/completion.h>

#include "abi/guc_relay_communication_abi.h"
#include "xe_sriov_trace_types.h"

struct xe_reg;
struct xe_gt;

/* One outstanding GGTT/MMIO transaction per VF and GT. */
struct xe_gt_sriov_pf_mmio {
	struct work_struct work;
	struct xe_gt *gt;
	u32 vfid;
	u32 msg[6];
	u32 ctb_msg[GUC_RELAY_MSG_MAX_LEN];
	u32 ctb_len;
	bool ctb;
	struct completion ctb_done;
	int ctb_result;
	atomic_t pending;
	atomic_t generation;
	int request_generation;
	int request_epoch;
	int primary_epoch;
	u64 trace_serial;
	struct xe_sriov_vf_trace trace;
};

/**
 * struct xe_gt_sriov_pf_service_version - VF/PF ABI Version.
 * @major: the major version of the VF/PF ABI
 * @minor: the minor version of the VF/PF ABI
 *
 * See `GuC Relay Communication`_.
 */
struct xe_gt_sriov_pf_service_version {
	u16 major;
	u16 minor;
};

/**
 * struct xe_gt_sriov_pf_service_runtime_regs - Runtime data shared with VFs.
 * @regs: pointer to static array with register offsets.
 * @values: pointer to array with captured register values.
 * @size: size of the regs and value arrays.
 */
struct xe_gt_sriov_pf_service_runtime_regs {
	const struct xe_reg *regs;
	u32 *values;
	u32 size;
};

/**
 * struct xe_gt_sriov_pf_service - Data used by the PF service.
 * @version: information about VF/PF ABI versions for current platform.
 * @version.base: lowest VF/PF ABI version that could be negotiated with VF.
 * @version.latest: latest VF/PF ABI version supported by the PF driver.
 * @runtime: runtime data shared with VFs.
 */
struct xe_gt_sriov_pf_service {
	struct {
		struct xe_gt_sriov_pf_service_version base;
		struct xe_gt_sriov_pf_service_version latest;
	} version;
	struct xe_gt_sriov_pf_service_runtime_regs runtime;
	struct workqueue_struct *mmio_wq;
	atomic_t mmio_epoch;
};

#endif
