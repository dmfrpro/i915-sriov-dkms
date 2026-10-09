// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023-2024 Intel Corporation
 */

#include <drm/drm_managed.h>
#include <linux/dma-fence.h>
#include <linux/delay.h>

#include "abi/guc_relay_actions_abi.h"
#include "abi/guc_mmio_relay_abi.h"
#include "abi/guc_ggtt_relay_abi.h"

#include "regs/xe_gsc_regs.h"
#include "regs/xe_gt_regs.h"
#include "regs/xe_guc_regs.h"

#include "xe_assert.h"
#include "xe_mmio.h"
#include "xe_gt_sriov_pf_helpers.h"
#include "xe_gt_sriov_pf_control.h"
#include "xe_ggtt.h"
#include "xe_pm.h"
#include "xe_gt_sriov_printk.h"
#include "xe_gt_sriov_pf_service.h"
#include "xe_gt_sriov_pf_service_types.h"
#include "xe_guc_ct.h"
#include "xe_guc_hxg_helpers.h"
#include "xe_sriov.h"
#include "xe_sriov_pf_service.h"
#include "xe_trace.h"

static void pf_mmio_worker(struct work_struct *work);

static void pf_trace_request(struct xe_gt_sriov_pf_mmio *mmio)
{
	struct xe_sriov_vf_trace *trace = &mmio->trace;
	u32 descriptor;

	*trace = (struct xe_sriov_vf_trace) {
		.id = ++mmio->trace_serial,
		.gt_id = mmio->gt->info.id,
		.vfid = mmio->vfid,
		.ctb = mmio->ctb,
		.generation = mmio->request_generation,
		.epoch = mmio->request_epoch,
		.primary_epoch = mmio->primary_epoch,
	};
	if (mmio->ctb) {
		trace->opcode = GUC_RELAY_ACTION_VF2PF_UPDATE_GGTT32;
		descriptor = mmio->ctb_msg[1];
		trace->offset = FIELD_GET(VF2PF_GGTT_OFFSET, descriptor);
		trace->mode = FIELD_GET(VF2PF_GGTT_MODE, descriptor);
		trace->copies = FIELD_GET(VF2PF_GGTT_COPIES, descriptor);
		trace->count = (mmio->ctb_len - 2) / 2;
	} else {
		trace->opcode = FIELD_GET(GUC_MMIO_RELAY_OPCODE, mmio->msg[2]);
		trace->token = FIELD_GET(GUC_MMIO_RELAY_MAGIC, mmio->msg[2]);
		if (trace->opcode == IOV_MMIO_UPDATE_GGTT) {
			descriptor = mmio->msg[3];
			trace->offset = FIELD_GET(IOV_MMIO_GGTT_OFFSET, descriptor);
			trace->mode = FIELD_GET(IOV_MMIO_GGTT_MODE, descriptor);
			trace->copies = FIELD_GET(IOV_MMIO_GGTT_COPIES, descriptor);
			trace->count = 1;
		}
	}
	trace_xe_sriov_vf_request(mmio->gt, trace, XE_SRIOV_TRACE_QUEUE, 0, 0, 0);
}

static void pf_trace_reject(struct xe_gt *gt, u32 vfid, bool ctb, u32 opcode, int err)
{
	struct xe_sriov_vf_trace trace = {
		.vfid = vfid,
		.ctb = ctb,
		.opcode = opcode,
	};

	trace_xe_sriov_vf_request(gt, &trace, XE_SRIOV_TRACE_REJECT, err, 0, 0);
}

static bool pf_mmio_current(struct xe_gt_sriov_pf_mmio *mmio)
{
	struct xe_gt *gt = mmio->gt;

	return mmio->request_generation == atomic_read(&mmio->generation) &&
	       mmio->request_epoch == atomic_read(&gt->sriov.pf.service.mmio_epoch) &&
	       mmio->primary_epoch ==
	       atomic_read(&gt->tile->primary_gt->sriov.pf.service.mmio_epoch) &&
	       !xe_gt_sriov_pf_control_flr_in_progress(gt, mmio->vfid) &&
	       !xe_gt_sriov_pf_control_flr_in_progress(gt->tile->primary_gt, mmio->vfid) &&
	       xe_guc_ct_enabled(&gt->uc.guc.ct);
}

void xe_gt_sriov_pf_service_stop_mmio(struct xe_gt *gt)
{
	if (IS_SRIOV_PF(gt_to_xe(gt)) && gt->sriov.pf.service.mmio_wq)
		atomic_inc(&gt->sriov.pf.service.mmio_epoch);
}

void xe_gt_sriov_pf_service_flush_mmio(struct xe_gt *gt)
{
	if (gt->sriov.pf.service.mmio_wq)
		flush_workqueue(gt->sriov.pf.service.mmio_wq);
}

void xe_gt_sriov_pf_service_reset_mmio_vf(struct xe_gt *gt, u32 vfid)
{
	if (gt->sriov.pf.service.mmio_wq)
		atomic_inc(&gt->sriov.pf.vfs[vfid].mmio.generation);
}

bool xe_gt_sriov_pf_service_mmio_busy(struct xe_gt *gt, u32 vfid)
{
	struct xe_gt *other;
	u32 id;

	for_each_gt(other, gt_to_xe(gt), id)
		if (other->sriov.pf.service.mmio_wq &&
		    atomic_read(&other->sriov.pf.vfs[vfid].mmio.pending))
			return true;
	return false;
}

/* Must be called without the PF master mutex or the CT mutex. */
void xe_gt_sriov_pf_service_flush_mmio_vf(struct xe_gt *gt, u32 vfid)
{
	struct xe_gt *other;
	u32 id;

	for_each_gt(other, gt_to_xe(gt), id)
		if (other->sriov.pf.service.mmio_wq)
			flush_work(&other->sriov.pf.vfs[vfid].mmio.work);
}

static int pf_mmio_runtime(struct xe_gt *gt, const u32 *msg, u32 *data)
{
	struct xe_gt_sriov_pf_service_runtime_regs *runtime;
	struct xe_gt *other;
	u32 i, j, id;
	bool found;

	if (!msg[3])
		return -EINVAL;
	for (i = 0; i < 3; i++) {
		if (!msg[i + 3])
			continue;
		found = false;
		for_each_gt(other, gt_to_xe(gt), id) {
			runtime = &other->sriov.pf.service.runtime;
			for (j = 0; j < runtime->size; j++) {
				if (msg[i + 3] != xe_mmio_adjusted_addr(&other->mmio,
								     runtime->regs[j].addr))
					continue;
				data[i + 1] = READ_ONCE(runtime->values[j]);
				found = true;
				break;
			}
			if (found)
				break;
		}
		if (!found) {
			xe_gt_sriov_notice(gt, "MMIO runtime offset %#x is not cached\n", msg[i + 3]);
			return -EACCES;
		}
	}
	return 0;
}

static int pf_update_vf_ggtt(struct xe_gt_sriov_pf_mmio *mmio, u32 offset,
			     u8 mode, u16 copies, const u64 *ptes, u32 count)
{
	struct xe_gt *gt = mmio->gt, *primary = gt->tile->primary_gt;
	struct xe_ggtt_node *node;
	struct dma_fence *fence;
	int err;

	if (MEDIA_VERx100(gt_to_xe(gt)) != 1300)
		return -EOPNOTSUPP;

	mutex_lock(xe_gt_sriov_pf_master_mutex(gt));
	node = primary->sriov.pf.vfs[mmio->vfid].config.ggtt_region;
	if (!pf_mmio_current(mmio) || !node) {
		mutex_unlock(xe_gt_sriov_pf_master_mutex(gt));
		return -ECANCELED;
	}
	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_GGTT_SUBMIT, 0, 0, 0);
	fence = xe_ggtt_update_vf_ptes(node, mmio->vfid, offset, mode, copies, ptes, count);
	mutex_unlock(xe_gt_sriov_pf_master_mutex(gt));
	if (IS_ERR(fence)) {
		trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_GGTT_DONE,
					  PTR_ERR(fence), 0, 0);
		return PTR_ERR(fence);
	}

	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_GGTT_FENCE,
				  0, fence->context, fence->seqno);
	/* Keep the VF region pinned until HW completes, also on a scheduler reset. */
	err = dma_fence_wait(fence, false);
	if (!err)
		err = dma_fence_get_status(fence);
	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_GGTT_DONE,
				  err, fence->context, fence->seqno);
	dma_fence_put(fence);
	if (err < 0)
		return err;
	if (!pf_mmio_current(mmio))
		return -ECANCELED;
	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_TLB_BEGIN, 0, 0, 0);
	err = xe_ggtt_invalidate_vf(gt->tile->mem.ggtt, &mmio->trace);
	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_TLB_DONE, err, 0, 0);
	if (err)
		return err;
	return copies + count;
}

static int pf_mmio_update_ggtt(struct xe_gt_sriov_pf_mmio *mmio, u32 *data)
{
	u32 *msg = mmio->msg;
	u64 pte = ((u64)msg[5] << 32) | msg[4];
	int ret;

	ret = pf_update_vf_ggtt(mmio, FIELD_GET(IOV_MMIO_GGTT_OFFSET, msg[3]),
				FIELD_GET(IOV_MMIO_GGTT_MODE, msg[3]),
				FIELD_GET(IOV_MMIO_GGTT_COPIES, msg[3]), &pte, 1);
	if (ret < 0)
		return ret;
	data[0] = ret;
	return 0;
}

static int pf_ctb_update_ggtt(struct xe_gt_sriov_pf_mmio *mmio)
{
	u64 *ptes;
	u32 *msg = mmio->ctb_msg;
	u32 count = (mmio->ctb_len - 2) / 2, i;
	int ret;

	ptes = kmalloc_array(count, sizeof(*ptes), GFP_KERNEL);
	if (!ptes)
		return -ENOMEM;
	for (i = 0; i < count; i++)
		ptes[i] = ((u64)msg[3 + 2 * i] << 32) | msg[2 + 2 * i];
	ret = pf_update_vf_ggtt(mmio, FIELD_GET(VF2PF_GGTT_OFFSET, msg[1]),
			      FIELD_GET(VF2PF_GGTT_MODE, msg[1]),
			      FIELD_GET(VF2PF_GGTT_COPIES, msg[1]), ptes, count);
	kfree(ptes);
	return ret;
}

static void pf_mmio_worker(struct work_struct *work)
{
	struct xe_gt_sriov_pf_mmio *mmio = container_of(work, typeof(*mmio), work);
	struct xe_gt *gt = mmio->gt;
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_guc_ct *ct = &gt->uc.guc.ct;
	u32 opcode = FIELD_GET(GUC_MMIO_RELAY_OPCODE, mmio->msg[2]);
	u32 magic = FIELD_GET(GUC_MMIO_RELAY_MAGIC, mmio->msg[2]);
	u32 data[4] = {}, reply[6] = {}, major, minor;
	u32 len, retry;
	int err = -ECANCELED, ret = 0;
	bool ctb = mmio->ctb;

	xe_pm_runtime_get(xe);
	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_WORK_BEGIN, 0, 0, 0);
	if (ctb) {
		if (pf_mmio_current(mmio))
			err = pf_ctb_update_ggtt(mmio);
		xe_gt_sriov_info(gt, "CTB GGTT VF%u offset %#x mode %u copies %u ptes %u result %d\n",
				 mmio->vfid, (u32)FIELD_GET(VF2PF_GGTT_OFFSET, mmio->ctb_msg[1]),
				 (u32)FIELD_GET(VF2PF_GGTT_MODE, mmio->ctb_msg[1]),
				 (u32)FIELD_GET(VF2PF_GGTT_COPIES, mmio->ctb_msg[1]),
				 (mmio->ctb_len - 2) / 2, err);
		mmio->ctb_result = err;
		goto done;
	}
	if (!pf_mmio_current(mmio))
		goto done;
	switch (opcode) {
	case IOV_MMIO_HANDSHAKE:
		if (mmio->msg[4] || mmio->msg[5]) {
			err = -EPROTO;
			break;
		}
		mutex_lock(xe_gt_sriov_pf_master_mutex(gt));
		err = xe_sriov_pf_service_handshake_vf(xe, mmio->vfid,
						     mmio->msg[3] >> 16,
						     mmio->msg[3] & 0xffff, &major, &minor);
		mutex_unlock(xe_gt_sriov_pf_master_mutex(gt));
		if (!err)
			data[1] = major << 16 | minor;
		break;
	case IOV_MMIO_GET_RUNTIME:
		err = pf_mmio_runtime(gt, mmio->msg, data);
		break;
	case IOV_MMIO_UPDATE_GGTT:
		err = pf_mmio_update_ggtt(mmio, data);
		break;
	default:
		err = -EOPNOTSUPP;
	}

	reply[0] = err ? GUC_ACTION_PF2GUC_MMIO_RELAY_FAILURE :
			 GUC_ACTION_PF2GUC_MMIO_RELAY_SUCCESS;
	reply[1] = mmio->vfid;
	reply[2] = FIELD_PREP(GUC_MMIO_RELAY_MAGIC, magic) |
		   (err ? min_t(u32, -err, 255) : data[0]);
	memcpy(reply + 3, data + 1, 3 * sizeof(u32));
	len = err ? 3 : 6;
	/* Never hold CT while waiting for the GPU or the PF configuration lock. */
	for (retry = 0; retry < 50; retry++) {
		mutex_lock(&ct->lock);
		if (!pf_mmio_current(mmio)) {
			mutex_unlock(&ct->lock);
			goto done;
		}
		ret = xe_guc_ct_send_g2h_handler_nowait(ct, reply, len);
		mutex_unlock(&ct->lock);
		if (ret != -EBUSY)
			break;
		msleep(20);
	}
	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_RESPONSE_SEND,
				  ret, 0, 0);
	if ((opcode == IOV_MMIO_HANDSHAKE || opcode == IOV_MMIO_GET_RUNTIME) &&
	    trace_xe_sriov_pf_service_reply_enabled()) {
		struct xe_sriov_pf_reply_trace meta = {
			.id = mmio->trace.id,
			.vfid = mmio->vfid,
			.token = magic,
			.action = opcode,
			.result = err,
			.send_result = ret,
		};

		trace_xe_sriov_pf_service_reply(gt, &meta, mmio->msg,
					      ARRAY_SIZE(mmio->msg), reply, len);
	}
	xe_gt_sriov_info(gt, "MMIO relay VF%u opcode %#x magic %#x result %d data0 %u send %d\n",
			 mmio->vfid, opcode, magic, err, data[0], ret);
done:
	trace_xe_sriov_vf_request(gt, &mmio->trace, XE_SRIOV_TRACE_WORK_DONE, err, 0, 0);
	xe_pm_runtime_put(xe);
	atomic_set(&mmio->pending, 0);
	if (ctb)
		complete(&mmio->ctb_done);
}

int xe_gt_sriov_pf_service_process_mmio(struct xe_gt *gt, const u32 *msg, u32 len)
{
	struct xe_gt_sriov_pf_mmio *mmio;
	u32 vfid;

	if (!IS_SRIOV_PF(gt_to_xe(gt)) || !gt->sriov.pf.service.mmio_wq)
		return -EOPNOTSUPP;
	if (len != GUC_MMIO_RELAY_EVENT_LEN || (msg[2] & GUC_MMIO_RELAY_MBZ) ||
	    FIELD_GET(GUC_HXG_MSG_0_ORIGIN, msg[0]) != GUC_HXG_ORIGIN_GUC ||
	    FIELD_GET(GUC_HXG_MSG_0_TYPE, msg[0]) != GUC_HXG_TYPE_EVENT)
		return -EPROTO;
	vfid = msg[1];
	if (!vfid || vfid > xe_gt_sriov_pf_get_totalvfs(gt))
		return -EINVAL;
	if (xe_gt_sriov_pf_control_flr_in_progress(gt, vfid)) {
		pf_trace_reject(gt, vfid, false,
				FIELD_GET(GUC_MMIO_RELAY_OPCODE, msg[2]), -ECANCELED);
		return 0;
	}
	mmio = &gt->sriov.pf.vfs[vfid].mmio;
	if (atomic_cmpxchg(&mmio->pending, 0, 1)) {
		pf_trace_reject(gt, vfid, false,
				FIELD_GET(GUC_MMIO_RELAY_OPCODE, msg[2]), -EBUSY);
		return 0;
	}
	mmio->ctb = false;
	memcpy(mmio->msg, msg, sizeof(mmio->msg));
	mmio->request_generation = atomic_read(&mmio->generation);
	mmio->request_epoch = atomic_read(&gt->sriov.pf.service.mmio_epoch);
	mmio->primary_epoch = atomic_read(&gt->tile->primary_gt->sriov.pf.service.mmio_epoch);
	pf_trace_request(mmio);
	xe_gt_sriov_info(gt, "MMIO relay VF%u queued len %u msg %*ph\n",
			 vfid, len, (int)sizeof(mmio->msg), msg);
	queue_work(gt->sriov.pf.service.mmio_wq, &mmio->work);
	return 0;
}

static int pf_process_update_ggtt_msg(struct xe_gt *gt, u32 origin,
				      const u32 *msg, u32 len, u32 *response, u32 size)
{
	struct xe_gt_sriov_pf_mmio *mmio;
	int generation, epoch, primary_epoch, ret;
	struct xe_sriov_vf_trace trace;

	if (!gt->sriov.pf.service.mmio_wq || MEDIA_VERx100(gt_to_xe(gt)) != 1300)
		return -EOPNOTSUPP;
	if (!origin || origin > xe_gt_sriov_pf_get_totalvfs(gt))
		return -EINVAL;
	if (!xe_sriov_pf_service_is_negotiated(gt_to_xe(gt), origin, 1, 0))
		return -EACCES;
	if (len < VF2PF_GGTT_REQUEST_MIN_LEN || (len & 1) ||
	    len > GUC_RELAY_MSG_MAX_LEN ||
	    FIELD_GET(GUC_HXG_REQUEST_MSG_0_DATA0, msg[0]))
		return -EPROTO;
	if (!size)
		return -ENOBUFS;
	mmio = &gt->sriov.pf.vfs[origin].mmio;
	if (atomic_cmpxchg(&mmio->pending, 0, 1)) {
		pf_trace_reject(gt, origin, true, GUC_RELAY_ACTION_VF2PF_UPDATE_GGTT32,
				-EINPROGRESS);
		return -EINPROGRESS;
	}
	/* Use the same work item so FLR/release waits for both transports. */
	mmio->ctb = true;
	mmio->ctb_len = len;
	memcpy(mmio->ctb_msg, msg, len * sizeof(u32));
	mmio->request_generation = generation = atomic_read(&mmio->generation);
	mmio->request_epoch = epoch = atomic_read(&gt->sriov.pf.service.mmio_epoch);
	mmio->primary_epoch = primary_epoch =
		atomic_read(&gt->tile->primary_gt->sriov.pf.service.mmio_epoch);
	pf_trace_request(mmio);
	trace = mmio->trace;
	reinit_completion(&mmio->ctb_done);
	queue_work(gt->sriov.pf.service.mmio_wq, &mmio->work);
	/* Called by the relay worker, outside the CT and PF configuration locks. */
	wait_for_completion(&mmio->ctb_done);
	ret = mmio->ctb_result;
	if (ret < 0) {
		trace_xe_sriov_vf_request(gt, &trace, XE_SRIOV_TRACE_RESPONSE_READY, ret, 0, 0);
		return ret;
	}
	if (generation != atomic_read(&mmio->generation) ||
	    epoch != atomic_read(&gt->sriov.pf.service.mmio_epoch) ||
	    primary_epoch != atomic_read(&gt->tile->primary_gt->sriov.pf.service.mmio_epoch) ||
	    xe_gt_sriov_pf_control_flr_in_progress(gt, origin) ||
	    xe_gt_sriov_pf_control_flr_in_progress(gt->tile->primary_gt, origin)) {
		trace_xe_sriov_vf_request(gt, &trace, XE_SRIOV_TRACE_RESPONSE_READY,
					  -ECANCELED, 0, 0);
		return -ECANCELED;
	}
	response[0] = FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		      FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_RESPONSE_SUCCESS) |
		      FIELD_PREP(GUC_HXG_RESPONSE_MSG_0_DATA0, ret);
	trace_xe_sriov_vf_request(gt, &trace, XE_SRIOV_TRACE_RESPONSE_READY, ret, 0, 0);
	return 1;
}

static const struct xe_reg tgl_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_SLICE_ENABLE,		/* _MMIO(0x9138) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	HUC_KERNEL_LOAD_INFO,		/* _MMIO(0xc1dc) */
};

static const struct xe_reg ats_m_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	MIRROR_FUSE1,			/* _MMIO(0x911c) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	HUC_KERNEL_LOAD_INFO,		/* _MMIO(0xc1dc) */
};

static const struct xe_reg pvc_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	XEHPC_GT_COMPUTE_DSS_ENABLE_EXT,/* _MMIO(0x9148) */
	HUC_KERNEL_LOAD_INFO,		/* _MMIO(0xc1dc) */
};

static const struct xe_reg ver_1270_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	XEHP_FUSE4,			/* _MMIO(0x9114) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	MIRROR_FUSE1,			/* _MMIO(0x911c) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	XEHPC_GT_COMPUTE_DSS_ENABLE_EXT,/* _MMIO(0x9148) */
	HUC_KERNEL_LOAD_INFO,		/* _MMIO(0xc1dc) */
};

/* Match the i915 MTL runtime coverage on the primary GT only. */
static const struct xe_reg mtl_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	XEHP_FUSE4,			/* _MMIO(0x9114) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	MIRROR_FUSE1,			/* _MMIO(0x911c) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	XEHPC_GT_COMPUTE_DSS_ENABLE_EXT, /* _MMIO(0x9148) */
	XE_REG(0xa26c),			/* CTC_MODE */
	HUC_KERNEL_LOAD_INFO,		/* _MMIO(0xc1dc) */
	XE_REG(0x44074),			/* Timestamp override */
	XE_REG(0x10100c),		/* Early runtime */
	HECI_FWSTS5(MTL_GSC_HECI1_BASE),	/* _MMIO(0x116c68) */
	XE_REG(0x138010),		/* MTL_GT_ACTIVITY_FACTOR */
};

static const struct xe_reg ver_2000_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	XEHP_FUSE4,			/* _MMIO(0x9114) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	MIRROR_FUSE1,			/* _MMIO(0x911c) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	XEHPC_GT_COMPUTE_DSS_ENABLE_EXT,/* _MMIO(0x9148) */
	XE2_GT_COMPUTE_DSS_2,		/* _MMIO(0x914c) */
	XE2_GT_GEOMETRY_DSS_1,		/* _MMIO(0x9150) */
	XE2_GT_GEOMETRY_DSS_2,		/* _MMIO(0x9154) */
	HUC_KERNEL_LOAD_INFO,		/* _MMIO(0xc1dc) */
};

static const struct xe_reg ver_3000_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	XEHP_FUSE4,			/* _MMIO(0x9114) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	MIRROR_FUSE1,			/* _MMIO(0x911c) */
	MIRROR_L3BANK_ENABLE,		/* _MMIO(0x9130) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	XEHPC_GT_COMPUTE_DSS_ENABLE_EXT,/* _MMIO(0x9148) */
	XE2_GT_COMPUTE_DSS_2,		/* _MMIO(0x914c) */
	XE2_GT_GEOMETRY_DSS_1,		/* _MMIO(0x9150) */
	XE2_GT_GEOMETRY_DSS_2,		/* _MMIO(0x9154) */
	HUC_KERNEL_LOAD_INFO,		/* _MMIO(0xc1dc) */
};

static const struct xe_reg ver_35_runtime_regs[] = {
	RPM_CONFIG0,			/* _MMIO(0x0d00) */
	XEHP_FUSE4,			/* _MMIO(0x9114) */
	MIRROR_FUSE3,			/* _MMIO(0x9118) */
	MIRROR_L3BANK_ENABLE,		/* _MMIO(0x9130) */
	XELP_EU_ENABLE,			/* _MMIO(0x9134) */
	XELP_GT_GEOMETRY_DSS_ENABLE,	/* _MMIO(0x913c) */
	GT_VEBOX_VDBOX_DISABLE,		/* _MMIO(0x9140) */
	XEHP_GT_COMPUTE_DSS_ENABLE,	/* _MMIO(0x9144) */
	XEHPC_GT_COMPUTE_DSS_ENABLE_EXT,/* _MMIO(0x9148) */
	XE2_GT_COMPUTE_DSS_2,		/* _MMIO(0x914c) */
	XE2_GT_GEOMETRY_DSS_1,		/* _MMIO(0x9150) */
	XE2_GT_GEOMETRY_DSS_2,		/* _MMIO(0x9154) */
	XE3P_XPC_GT_GEOMETRY_DSS_3,	/* _MMIO(0x915c) */
	XE3P_XPC_GT_COMPUTE_DSS_3,	/* _MMIO(0x9160) */
	SERVICE_COPY_ENABLE,		/* _MMIO(0x9170) */
};

static const struct xe_reg *pick_runtime_regs(struct xe_gt *gt, unsigned int *count)
{
	struct xe_device *xe = gt_to_xe(gt);
	const struct xe_reg *regs;

	if (xe->info.platform == XE_METEORLAKE && gt->info.type == XE_GT_TYPE_MAIN) {
		*count = ARRAY_SIZE(mtl_runtime_regs);
		regs = mtl_runtime_regs;
	} else if (GRAPHICS_VER(xe) >= 35) {
		*count = ARRAY_SIZE(ver_35_runtime_regs);
		regs = ver_35_runtime_regs;
	} else if (GRAPHICS_VERx100(xe) >= 3000) {
		*count = ARRAY_SIZE(ver_3000_runtime_regs);
		regs = ver_3000_runtime_regs;
	} else if (GRAPHICS_VERx100(xe) >= 2000) {
		*count = ARRAY_SIZE(ver_2000_runtime_regs);
		regs = ver_2000_runtime_regs;
	} else if (GRAPHICS_VERx100(xe) >= 1270) {
		*count = ARRAY_SIZE(ver_1270_runtime_regs);
		regs = ver_1270_runtime_regs;
	} else if (GRAPHICS_VERx100(xe) == 1260) {
		*count = ARRAY_SIZE(pvc_runtime_regs);
		regs = pvc_runtime_regs;
	} else if (GRAPHICS_VERx100(xe) == 1255) {
		*count = ARRAY_SIZE(ats_m_runtime_regs);
		regs = ats_m_runtime_regs;
	} else if (GRAPHICS_VERx100(xe) == 1200) {
		*count = ARRAY_SIZE(tgl_runtime_regs);
		regs = tgl_runtime_regs;
	} else {
		regs = ERR_PTR(-ENOPKG);
		*count = 0;
	}

	return regs;
}

static int pf_alloc_runtime_info(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	const struct xe_reg *regs;
	unsigned int size;
	u32 *values;

	xe_gt_assert(gt, IS_SRIOV_PF(xe));
	xe_gt_assert(gt, !gt->sriov.pf.service.runtime.size);
	xe_gt_assert(gt, !gt->sriov.pf.service.runtime.regs);
	xe_gt_assert(gt, !gt->sriov.pf.service.runtime.values);

	regs = pick_runtime_regs(gt, &size);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	if (unlikely(!size))
		return 0;

	values = drmm_kcalloc(&xe->drm, size, sizeof(u32), GFP_KERNEL);
	if (!values)
		return -ENOMEM;

	gt->sriov.pf.service.runtime.size = size;
	gt->sriov.pf.service.runtime.regs = regs;
	gt->sriov.pf.service.runtime.values = values;

	return 0;
}

static void read_many(struct xe_gt *gt, unsigned int count,
		      const struct xe_reg *regs, u32 *values)
{
	while (count--)
		*values++ = xe_mmio_read32(&gt->mmio, *regs++);
}

static void pf_prepare_runtime_info(struct xe_gt *gt)
{
	const struct xe_reg *regs;
	unsigned int size;
	u32 *values;

	if (!gt->sriov.pf.service.runtime.size)
		return;

	size = gt->sriov.pf.service.runtime.size;
	regs = gt->sriov.pf.service.runtime.regs;
	values = gt->sriov.pf.service.runtime.values;

	read_many(gt, size, regs, values);

	if (IS_ENABLED(CONFIG_DRM_XE_DEBUG_SRIOV)) {
		struct drm_printer p = xe_gt_dbg_printer(gt);

		xe_gt_sriov_pf_service_print_runtime(gt, &p);
	}
}

/**
 * xe_gt_sriov_pf_service_init - Early initialization of the GT SR-IOV PF services.
 * @gt: the &xe_gt to initialize
 *
 * Performs early initialization of the GT SR-IOV PF services, including preparation
 * of the runtime info that will be shared with VFs.
 *
 * This function can only be called on PF.
 */
int xe_gt_sriov_pf_service_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	u32 vfid;
	int err;

	err = pf_alloc_runtime_info(gt);
	if (unlikely(err))
		goto failed;
	/* This experiment is restricted to MTL/ARL's shared descriptor. */
	if (xe->info.platform == XE_METEORLAKE) {
		gt->sriov.pf.service.mmio_wq =
			drmm_alloc_ordered_workqueue(&xe->drm, "xe-pf-mmio", WQ_MEM_RECLAIM);
		if (IS_ERR(gt->sriov.pf.service.mmio_wq)) {
			err = PTR_ERR(gt->sriov.pf.service.mmio_wq);
			gt->sriov.pf.service.mmio_wq = NULL;
			goto failed;
		}
		atomic_set(&gt->sriov.pf.service.mmio_epoch, 0);
		for (vfid = 1; vfid <= xe_gt_sriov_pf_get_totalvfs(gt); vfid++) {
			struct xe_gt_sriov_pf_mmio *mmio = &gt->sriov.pf.vfs[vfid].mmio;

			mmio->gt = gt;
			mmio->vfid = vfid;
			mmio->trace_serial = 0;
			atomic_set(&mmio->pending, 0);
			atomic_set(&mmio->generation, 0);
			INIT_WORK(&mmio->work, pf_mmio_worker);
			init_completion(&mmio->ctb_done);
		}
	}

	return 0;
failed:
	xe_gt_sriov_err(gt, "Failed to initialize service (%pe)\n", ERR_PTR(err));
	return err;
}

/**
 * xe_gt_sriov_pf_service_update - Update PF SR-IOV services.
 * @gt: the &xe_gt to update
 *
 * Updates runtime data shared with VFs.
 *
 * This function can be called more than once.
 * This function can only be called on PF.
 */
void xe_gt_sriov_pf_service_update(struct xe_gt *gt)
{
	pf_prepare_runtime_info(gt);
}

/* Return: length of the response message or a negative error code on failure. */
static int pf_process_handshake_msg(struct xe_gt *gt, u32 origin,
				    const u32 *request, u32 len, u32 *response, u32 size)
{
	u32 wanted_major, wanted_minor;
	u32 major, minor;
	u32 mbz;
	int err;

	if (unlikely(len != VF2PF_HANDSHAKE_REQUEST_MSG_LEN))
		return -EMSGSIZE;

	mbz = FIELD_GET(VF2PF_HANDSHAKE_REQUEST_MSG_0_MBZ, request[0]);
	if (unlikely(mbz))
		return -EPFNOSUPPORT;

	wanted_major = FIELD_GET(VF2PF_HANDSHAKE_REQUEST_MSG_1_MAJOR, request[1]);
	wanted_minor = FIELD_GET(VF2PF_HANDSHAKE_REQUEST_MSG_1_MINOR, request[1]);

	err = xe_sriov_pf_service_handshake_vf(gt_to_xe(gt), origin, wanted_major, wanted_minor,
					       &major, &minor);
	if (err < 0)
		return err;

	xe_gt_assert(gt, major || minor);
	xe_gt_assert(gt, size >= VF2PF_HANDSHAKE_RESPONSE_MSG_LEN);

	response[0] = FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		      FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_RESPONSE_SUCCESS) |
		      FIELD_PREP(GUC_HXG_RESPONSE_MSG_0_DATA0, 0);
	response[1] = FIELD_PREP(VF2PF_HANDSHAKE_RESPONSE_MSG_1_MAJOR, major) |
		      FIELD_PREP(VF2PF_HANDSHAKE_RESPONSE_MSG_1_MINOR, minor);

	return VF2PF_HANDSHAKE_RESPONSE_MSG_LEN;
}

struct reg_data {
	u32 offset;
	u32 value;
} __packed;
static_assert(hxg_sizeof(struct reg_data) == 2);

/* Return: number of entries copied or negative error code on failure. */
static int pf_service_runtime_query(struct xe_gt *gt, u32 start, u32 limit,
				    struct reg_data *data, u32 *remaining)
{
	struct xe_gt_sriov_pf_service_runtime_regs *runtime;
	unsigned int count, i;
	u32 addr;

	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));

	runtime = &gt->sriov.pf.service.runtime;

	if (start > runtime->size)
		return -ERANGE;

	count = min_t(u32, runtime->size - start, limit);

	for (i = 0; i < count; ++i, ++data) {
		addr = runtime->regs[start + i].addr;
		data->offset = xe_mmio_adjusted_addr(&gt->mmio, addr);
		data->value = runtime->values[start + i];
	}

	*remaining = runtime->size - start - count;
	return count;
}

/* Return: length of the response message or a negative error code on failure. */
static int pf_process_runtime_query_msg(struct xe_gt *gt, u32 origin,
					const u32 *msg, u32 msg_len, u32 *response, u32 resp_size)
{
	const u32 chunk_size = hxg_sizeof(struct reg_data);
	struct reg_data *reg_data_buf;
	u32 limit, start, max_chunks;
	u32 remaining = 0;
	int ret;

	/* this action is available from ABI 1.0 */
	if (!xe_sriov_pf_service_is_negotiated(gt_to_xe(gt), origin, 1, 0))
		return -EACCES;

	if (unlikely(msg_len > VF2PF_QUERY_RUNTIME_REQUEST_MSG_LEN))
		return -EMSGSIZE;
	if (unlikely(msg_len < VF2PF_QUERY_RUNTIME_REQUEST_MSG_LEN))
		return -EPROTO;
	if (unlikely(resp_size < VF2PF_QUERY_RUNTIME_RESPONSE_MSG_MIN_LEN))
		return -EINVAL;

	limit = FIELD_GET(VF2PF_QUERY_RUNTIME_REQUEST_MSG_0_LIMIT, msg[0]);
	start = FIELD_GET(VF2PF_QUERY_RUNTIME_REQUEST_MSG_1_START, msg[1]);

	resp_size = min_t(u32, resp_size, VF2PF_QUERY_RUNTIME_RESPONSE_MSG_MAX_LEN);
	max_chunks = (resp_size - VF2PF_QUERY_RUNTIME_RESPONSE_MSG_MIN_LEN) / chunk_size;
	limit = limit == VF2PF_QUERY_RUNTIME_NO_LIMIT ? max_chunks : min_t(u32, max_chunks, limit);
	reg_data_buf = (void *)(response + VF2PF_QUERY_RUNTIME_RESPONSE_MSG_MIN_LEN);

	ret = pf_service_runtime_query(gt, start, limit, reg_data_buf, &remaining);
	if (ret < 0)
		return ret;

	response[0] = FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		      FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_RESPONSE_SUCCESS) |
		      FIELD_PREP(VF2PF_QUERY_RUNTIME_RESPONSE_MSG_0_COUNT, ret);
	response[1] = FIELD_PREP(VF2PF_QUERY_RUNTIME_RESPONSE_MSG_1_REMAINING, remaining);

	return VF2PF_QUERY_RUNTIME_RESPONSE_MSG_MIN_LEN + ret * hxg_sizeof(struct reg_data);
}

/**
 * xe_gt_sriov_pf_service_process_request - Service GT level SR-IOV request message from the VF.
 * @gt: the &xe_gt that provides the service
 * @origin: VF number that is requesting the service
 * @msg: request message
 * @msg_len: length of the request message (in dwords)
 * @response: placeholder for the response message
 * @resp_size: length of the response message buffer (in dwords)
 *
 * This function processes `Relay Message`_ request from the VF.
 *
 * Return: length of the response message or a negative error code on failure.
 */
int xe_gt_sriov_pf_service_process_request(struct xe_gt *gt, u32 origin,
					   const u32 *msg, u32 msg_len,
					   u32 *response, u32 resp_size)
{
	u32 action, data __maybe_unused;
	int ret;

	xe_gt_assert(gt, msg_len >= GUC_HXG_MSG_MIN_LEN);
	xe_gt_assert(gt, FIELD_GET(GUC_HXG_MSG_0_TYPE, msg[0]) == GUC_HXG_TYPE_REQUEST);

	action = FIELD_GET(GUC_HXG_REQUEST_MSG_0_ACTION, msg[0]);
	data = FIELD_GET(GUC_HXG_REQUEST_MSG_0_DATA0, msg[0]);
	xe_gt_sriov_dbg_verbose(gt, "service action %#x:%u from VF%u\n",
				action, data, origin);

	switch (action) {
	case GUC_RELAY_ACTION_VF2PF_HANDSHAKE:
		ret = pf_process_handshake_msg(gt, origin, msg, msg_len, response, resp_size);
		break;
	case GUC_RELAY_ACTION_VF2PF_QUERY_RUNTIME:
		ret = pf_process_runtime_query_msg(gt, origin, msg, msg_len, response, resp_size);
		break;
	case GUC_RELAY_ACTION_VF2PF_UPDATE_GGTT32:
		ret = pf_process_update_ggtt_msg(gt, origin, msg, msg_len, response, resp_size);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

/**
 * xe_gt_sriov_pf_service_print_runtime - Print PF runtime data shared with VFs.
 * @gt: the &xe_gt
 * @p: the &drm_printer
 *
 * This function is for PF use only.
 */
int xe_gt_sriov_pf_service_print_runtime(struct xe_gt *gt, struct drm_printer *p)
{
	const struct xe_reg *regs;
	unsigned int size;
	u32 *values;

	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));

	size = gt->sriov.pf.service.runtime.size;
	regs = gt->sriov.pf.service.runtime.regs;
	values = gt->sriov.pf.service.runtime.values;

	for (; size--; regs++, values++) {
		drm_printf(p, "reg[%#x] = %#x\n",
			   xe_mmio_adjusted_addr(&gt->mmio, regs->addr), *values);
	}

	return 0;
}
