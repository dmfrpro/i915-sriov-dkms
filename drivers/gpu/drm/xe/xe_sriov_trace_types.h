/* SPDX-License-Identifier: MIT */
#ifndef _XE_SRIOV_TRACE_TYPES_H_
#define _XE_SRIOV_TRACE_TYPES_H_

#include <linux/types.h>

enum xe_sriov_trace_phase {
	XE_SRIOV_TRACE_QUEUE,
	XE_SRIOV_TRACE_WORK_BEGIN,
	XE_SRIOV_TRACE_GGTT_SUBMIT,
	XE_SRIOV_TRACE_GGTT_DONE,
	XE_SRIOV_TRACE_TLB_BEGIN,
	XE_SRIOV_TRACE_TLB_DONE,
	XE_SRIOV_TRACE_RESPONSE_READY,
	XE_SRIOV_TRACE_RESPONSE_SEND,
	XE_SRIOV_TRACE_WORK_DONE,
	XE_SRIOV_TRACE_REJECT,
	XE_SRIOV_TRACE_RELAY_RECEIVE,
	XE_SRIOV_TRACE_RELAY_HANDLE,
	XE_SRIOV_TRACE_RELAY_RESULT,
	XE_SRIOV_TRACE_GGTT_FENCE,
};

/* Owned by the outstanding request, then copied before its worker is queued. */
struct xe_sriov_vf_trace {
	u64 id;
	u32 vfid;
	u32 opcode;
	u32 token;
	u32 offset;
	u32 count;
	u16 copies;
	u8 gt_id;
	u8 mode;
	bool ctb;
	int generation;
	int epoch;
	int primary_epoch;
};

/* A send attempt to GuC, not an acknowledgment from the guest. */
struct xe_sriov_pf_reply_trace {
	u64 id;
	u32 vfid;
	u32 token;
	u32 action;
	bool ctb;
	int result;
	int send_result;
};

#endif
