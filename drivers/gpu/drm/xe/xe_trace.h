/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2022 Intel Corporation
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM xe

#if !defined(_XE_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _XE_TRACE_H_

#include <linux/tracepoint.h>
#include <linux/types.h>

#include "xe_exec_queue_types.h"
#include "xe_exec_queue.h"
#include "xe_gpu_scheduler_types.h"
#include "xe_gt_types.h"
#include "xe_guc_exec_queue_types.h"
#include "xe_sched_job.h"
#include "xe_tlb_inval_types.h"
#include "xe_vm.h"
#include "xe_sriov_trace_types.h"

#define __dev_name_xe(xe)	dev_name((xe)->drm.dev)
#define __dev_name_tile(tile)	__dev_name_xe(tile_to_xe((tile)))
#define __dev_name_gt(gt)	__dev_name_xe(gt_to_xe((gt)))
#define __dev_name_eq(q)	__dev_name_gt((q)->gt)

DECLARE_EVENT_CLASS(xe_tlb_inval_fence,
		    TP_PROTO(struct xe_device *xe, struct xe_tlb_inval_fence *fence),
		    TP_ARGS(xe, fence),

		    TP_STRUCT__entry(
			     __string(dev, __dev_name_xe(xe))
			     __field(struct xe_tlb_inval_fence *, fence)
			     __field(int, seqno)
			     __field(u8, gt)
			     __field(u8, origin_gt)
			     __field(u32, vfid)
			     __field(u64, txn)
			     __field(u32, op)
			     __field(u32, asid)
			     __field(u64, start)
			     __field(u64, end)
			     __field(int, error)
			     ),

		    TP_fast_assign(
			   __assign_str(dev);
			   __entry->fence = fence;
			   __entry->seqno = fence->seqno;
			   __entry->gt = fence->tlb_inval->trace_gt_id;
			   __entry->origin_gt = fence->trace_origin_gt;
			   __entry->vfid = fence->trace_vfid;
			   __entry->txn = fence->trace_id;
			   __entry->op = fence->trace_op;
			   __entry->asid = fence->trace_asid;
			   __entry->start = fence->trace_start;
			   __entry->end = fence->trace_end;
			   __entry->error = fence->base.error;
			   ),

		    TP_printk("dev=%s fence=%p seqno=%d gt=%u origin_gt=%u vfid=%u txn=%llu op=%u asid=%u start=%#llx end=%#llx error=%d",
			      __get_str(dev), __entry->fence, __entry->seqno, __entry->gt,
			      __entry->origin_gt, __entry->vfid, __entry->txn, __entry->op,
			      __entry->asid,
			      __entry->start, __entry->end, __entry->error)
);

DEFINE_EVENT(xe_tlb_inval_fence, xe_tlb_inval_fence_send,
	     TP_PROTO(struct xe_device *xe, struct xe_tlb_inval_fence *fence),
	     TP_ARGS(xe, fence)
);

DEFINE_EVENT(xe_tlb_inval_fence, xe_tlb_inval_fence_recv,
	     TP_PROTO(struct xe_device *xe, struct xe_tlb_inval_fence *fence),
	     TP_ARGS(xe, fence)
);

DEFINE_EVENT(xe_tlb_inval_fence, xe_tlb_inval_fence_signal,
	     TP_PROTO(struct xe_device *xe, struct xe_tlb_inval_fence *fence),
	     TP_ARGS(xe, fence)
);

DEFINE_EVENT(xe_tlb_inval_fence, xe_tlb_inval_fence_timeout,
	     TP_PROTO(struct xe_device *xe, struct xe_tlb_inval_fence *fence),
	     TP_ARGS(xe, fence)
);

TRACE_EVENT(xe_sriov_ggtt_assign,
	TP_PROTO(struct xe_gt *gt, u16 vfid, u64 start, u64 size, bool gpu,
		 bool done, u32 completed, int result),
	TP_ARGS(gt, vfid, start, size, gpu, done, completed, result),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u16, vfid)
		__field(u64, start)
		__field(u64, size)
		__field(bool, gpu)
		__field(bool, done)
		__field(u32, completed)
		__field(int, result)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->vfid = vfid;
		__entry->start = start;
		__entry->size = size;
		__entry->gpu = gpu;
		__entry->done = done;
		__entry->completed = completed;
		__entry->result = result;
	),
	TP_printk("dev=%s gt=%u vfid=%u start=%#llx size=%#llx gpu=%u done=%u completed=%u result=%d",
		  __get_str(dev), __entry->gt, __entry->vfid, __entry->start,
		  __entry->size, __entry->gpu, __entry->done, __entry->completed,
		  __entry->result)
);

TRACE_EVENT(xe_sriov_vf_request,
	TP_PROTO(struct xe_gt *gt, const struct xe_sriov_vf_trace *trace,
		 enum xe_sriov_trace_phase phase, int result, u64 context, u64 seqno),
	TP_ARGS(gt, trace, phase, result, context, seqno),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u32, vfid)
		__field(u64, txn)
		__field(u32, opcode)
		__field(u32, token)
		__field(u32, offset)
		__field(u32, count)
		__field(u16, copies)
		__field(u8, mode)
		__field(bool, ctb)
		__field(int, generation)
		__field(int, epoch)
		__field(int, primary_epoch)
		__field(u32, phase)
		__field(int, result)
		__field(u64, context)
		__field(u64, seqno)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->vfid = trace->vfid;
		__entry->txn = trace->id;
		__entry->opcode = trace->opcode;
		__entry->token = trace->token;
		__entry->offset = trace->offset;
		__entry->count = trace->count;
		__entry->copies = trace->copies;
		__entry->mode = trace->mode;
		__entry->ctb = trace->ctb;
		__entry->generation = trace->generation;
		__entry->epoch = trace->epoch;
		__entry->primary_epoch = trace->primary_epoch;
		__entry->phase = phase;
		__entry->result = result;
		__entry->context = context;
		__entry->seqno = seqno;
	),
	TP_printk("dev=%s gt=%u vfid=%u txn=%llu ctb=%u opcode=%#x token=%#x phase=%u offset=%#x count=%u copies=%u mode=%u generation=%d epoch=%d primary_epoch=%d result=%d context=%llu seqno=%llu",
		  __get_str(dev), __entry->gt, __entry->vfid, __entry->txn,
		  __entry->ctb, __entry->opcode, __entry->token, __entry->phase,
		  __entry->offset, __entry->count, __entry->copies, __entry->mode,
		  __entry->generation, __entry->epoch, __entry->primary_epoch,
		  __entry->result, __entry->context, __entry->seqno)
);

TRACE_EVENT(xe_sriov_relay,
	TP_PROTO(struct xe_gt *gt, u32 vfid, u32 rid, u32 action,
		 enum xe_sriov_trace_phase phase, int result),
	TP_ARGS(gt, vfid, rid, action, phase, result),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u32, vfid)
		__field(u32, rid)
		__field(u32, action)
		__field(u32, phase)
		__field(int, result)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->vfid = vfid;
		__entry->rid = rid;
		__entry->action = action;
		__entry->phase = phase;
		__entry->result = result;
	),
	TP_printk("dev=%s gt=%u vfid=%u rid=%u action=%#x phase=%u result=%d",
		  __get_str(dev), __entry->gt, __entry->vfid, __entry->rid,
		  __entry->action, __entry->phase, __entry->result)
);

TRACE_EVENT(xe_sriov_guc_params,
	TP_PROTO(struct xe_gt *gt, u32 wa, u32 feature, u32 ads),
	TP_ARGS(gt, wa, feature, ads),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u32, wa)
		__field(u32, feature)
		__field(u32, ads)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->wa = wa;
		__entry->feature = feature;
		__entry->ads = ads;
	),
	TP_printk("dev=%s gt=%u wa=%#x feature=%#x ads=%#x",
		  __get_str(dev), __entry->gt, __entry->wa,
		  __entry->feature, __entry->ads)
);

TRACE_EVENT(xe_sriov_pf_wa_klv,
	TP_PROTO(struct xe_gt *gt, u32 key, const u32 *data, u32 len, int result),
	TP_ARGS(gt, key, data, len, result),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u32, key)
		__field(u32, len)
		__field(u32, copied)
		__field(int, result)
		__array(u32, data, 16)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->key = key;
		__entry->len = len;
		__entry->copied = min_t(u32, len, 16);
		__entry->result = result;
		memset(__entry->data, 0, sizeof(__entry->data));
		if (__entry->copied)
			memcpy(__entry->data, data, __entry->copied * sizeof(u32));
	),
	TP_printk("dev=%s gt=%u key=%#x len=%u copied=%u result=%d data=%s",
		  __get_str(dev), __entry->gt, __entry->key, __entry->len,
		  __entry->copied, __entry->result,
		  __print_array(__entry->data, __entry->copied, sizeof(u32)))
);

TRACE_EVENT(xe_sriov_pf_guc_event,
	TP_PROTO(struct xe_gt *gt, u32 action, bool handled, int result,
		 const u32 *msg, u32 len),
	TP_ARGS(gt, action, handled, result, msg, len),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u32, action)
		__field(bool, handled)
		__field(int, result)
		__field(u32, len)
		__field(u32, copied)
		__array(u32, msg, 16)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->action = action;
		__entry->handled = handled;
		__entry->result = result;
		__entry->len = len;
		__entry->copied = min_t(u32, len, 16);
		memset(__entry->msg, 0, sizeof(__entry->msg));
		if (__entry->copied)
			memcpy(__entry->msg, msg, __entry->copied * sizeof(u32));
	),
	TP_printk("dev=%s gt=%u action=%#x handled=%u result=%d len=%u copied=%u msg=%s",
		  __get_str(dev), __entry->gt, __entry->action, __entry->handled,
		  __entry->result, __entry->len, __entry->copied,
		  __print_array(__entry->msg, __entry->copied, sizeof(u32)))
);

TRACE_EVENT(xe_sriov_pf_vf_state,
	TP_PROTO(struct xe_gt *gt, u32 vfid, u32 bit, bool enter, unsigned long state),
	TP_ARGS(gt, vfid, bit, enter, state),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u32, vfid)
		__field(u32, bit)
		__field(bool, enter)
		__field(unsigned long, state)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->vfid = vfid;
		__entry->bit = bit;
		__entry->enter = enter;
		__entry->state = state;
	),
	TP_printk("dev=%s gt=%u vfid=%u bit=%u enter=%u state=%#lx",
		  __get_str(dev), __entry->gt, __entry->vfid, __entry->bit,
		  __entry->enter, __entry->state)
);

TRACE_EVENT(xe_sriov_pf_adverse,
	TP_PROTO(struct xe_gt *gt, u32 vfid, u32 key, u32 threshold, int result),
	TP_ARGS(gt, vfid, key, threshold, result),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u32, vfid)
		__field(u32, key)
		__field(u32, threshold)
		__field(int, result)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->vfid = vfid;
		__entry->key = key;
		__entry->threshold = threshold;
		__entry->result = result;
	),
	TP_printk("dev=%s gt=%u vfid=%u key=%#x threshold=%u result=%d",
		  __get_str(dev), __entry->gt, __entry->vfid, __entry->key,
		  __entry->threshold, __entry->result)
);

TRACE_EVENT(xe_sriov_pf_service_reply,
	TP_PROTO(struct xe_gt *gt, const struct xe_sriov_pf_reply_trace *meta,
		 const u32 *request, u32 request_len, const u32 *reply, u32 reply_len),
	TP_ARGS(gt, meta, request, request_len, reply, reply_len),
	TP_STRUCT__entry(
		__string(dev, __dev_name_gt(gt))
		__field(u8, gt)
		__field(u64, id)
		__field(u32, vfid)
		__field(u32, token)
		__field(u32, action)
		__field(bool, ctb)
		__field(int, result)
		__field(int, send_result)
		__field(u32, request_len)
		__field(u32, request_copied)
		__field(u32, reply_len)
		__field(u32, reply_copied)
		__array(u32, request, 8)
		__array(u32, reply, 64)
	),
	TP_fast_assign(
		__assign_str(dev);
		__entry->gt = gt->info.id;
		__entry->id = meta->id;
		__entry->vfid = meta->vfid;
		__entry->token = meta->token;
		__entry->action = meta->action;
		__entry->ctb = meta->ctb;
		__entry->result = meta->result;
		__entry->send_result = meta->send_result;
		__entry->request_len = request_len;
		__entry->request_copied = min_t(u32, request_len, 8);
		__entry->reply_len = reply_len;
		__entry->reply_copied = min_t(u32, reply_len, 64);
		memset(__entry->request, 0, sizeof(__entry->request));
		memset(__entry->reply, 0, sizeof(__entry->reply));
		if (__entry->request_copied)
			memcpy(__entry->request, request,
			       __entry->request_copied * sizeof(u32));
		if (__entry->reply_copied)
			memcpy(__entry->reply, reply, __entry->reply_copied * sizeof(u32));
	),
	TP_printk("dev=%s gt=%u vfid=%u ctb=%u id=%llu token=%#x action=%#x result=%d send_result=%d request_len=%u request_copied=%u reply_len=%u reply_copied=%u request=%s reply=%s",
		  __get_str(dev), __entry->gt, __entry->vfid, __entry->ctb,
		  __entry->id, __entry->token, __entry->action, __entry->result,
		  __entry->send_result, __entry->request_len, __entry->request_copied,
		  __entry->reply_len, __entry->reply_copied,
		  __print_array(__entry->request, __entry->request_copied, sizeof(u32)),
		  __print_array(__entry->reply, __entry->reply_copied, sizeof(u32)))
);

DECLARE_EVENT_CLASS(xe_exec_queue,
		    TP_PROTO(struct xe_exec_queue *q),
		    TP_ARGS(q),

		    TP_STRUCT__entry(
			     __string(dev, __dev_name_eq(q))
			     __field(enum xe_engine_class, class)
			     __field(u32, logical_mask)
			     __field(u8, gt_id)
			     __field(u16, width)
			     __field(u16, guc_id)
			     __field(u32, guc_state)
			     __field(u32, flags)
			     ),

		    TP_fast_assign(
			   __assign_str(dev);
			   __entry->class = q->class;
			   __entry->logical_mask = q->logical_mask;
			   __entry->gt_id = q->gt->info.id;
			   __entry->width = q->width;
			   __entry->guc_id = q->guc->id;
			   __entry->guc_state = atomic_read(&q->guc->state);
			   __entry->flags = q->flags;
			   ),

		    TP_printk("dev=%s, %d:0x%x, gt=%d, width=%d, guc_id=%d, guc_state=0x%x, flags=0x%x",
			      __get_str(dev), __entry->class, __entry->logical_mask,
			      __entry->gt_id, __entry->width, __entry->guc_id,
			      __entry->guc_state, __entry->flags)
);

DECLARE_EVENT_CLASS(xe_exec_queue_multi_queue,
		    TP_PROTO(struct xe_exec_queue *q),
		    TP_ARGS(q),

		    TP_STRUCT__entry(
			     __string(dev, __dev_name_eq(q))
			     __field(enum xe_engine_class, class)
			     __field(u32, logical_mask)
			     __field(u8, gt_id)
			     __field(u16, width)
			     __field(u32, guc_id)
			     __field(u32, guc_state)
			     __field(u32, flags)
			     __field(u32, primary)
			     ),

		    TP_fast_assign(
			   __assign_str(dev);
			   __entry->class = q->class;
			   __entry->logical_mask = q->logical_mask;
			   __entry->gt_id = q->gt->info.id;
			   __entry->width = q->width;
			   __entry->guc_id = q->guc->id;
			   __entry->guc_state = atomic_read(&q->guc->state);
			   __entry->flags = q->flags;
			   __entry->primary = xe_exec_queue_multi_queue_primary(q)->guc->id;
			   ),

		    TP_printk("dev=%s, %d:0x%x, gt=%d, width=%d guc_id=%d, guc_state=0x%x, flags=0x%x, primary=%d",
			      __get_str(dev), __entry->class, __entry->logical_mask,
			      __entry->gt_id, __entry->width, __entry->guc_id,
			      __entry->guc_state, __entry->flags,
			      __entry->primary)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_create,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue_multi_queue, xe_exec_queue_create_multi_queue,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_supress_resume,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_submit,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_scheduling_enable,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_scheduling_disable,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_scheduling_done,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_register,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_deregister,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_deregister_done,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_close,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_kill,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_cleanup_entity,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_destroy,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_reset,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_memory_cat_error,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_cgp_context_error,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_stop,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DEFINE_EVENT(xe_exec_queue, xe_exec_queue_resubmit,
	     TP_PROTO(struct xe_exec_queue *q),
	     TP_ARGS(q)
);

DECLARE_EVENT_CLASS(xe_sched_job,
		    TP_PROTO(struct xe_sched_job *job),
		    TP_ARGS(job),

		    TP_STRUCT__entry(
			     __string(dev, __dev_name_eq(job->q))
			     __field(u32, seqno)
			     __field(u32, lrc_seqno)
			     __field(u8, gt_id)
			     __field(u16, guc_id)
			     __field(u32, guc_state)
			     __field(u32, flags)
			     __field(int, error)
			     __field(struct dma_fence *, fence)
			     __field(u64, batch_addr)
			     ),

		    TP_fast_assign(
			   __assign_str(dev);
			   __entry->seqno = xe_sched_job_seqno(job);
			   __entry->lrc_seqno = xe_sched_job_lrc_seqno(job);
			   __entry->gt_id = job->q->gt->info.id;
			   __entry->guc_id = job->q->guc->id;
			   __entry->guc_state =
			   atomic_read(&job->q->guc->state);
			   __entry->flags = job->q->flags;
			   __entry->error = job->fence ? job->fence->error : 0;
			   __entry->fence = job->fence;
			   __entry->batch_addr = (u64)job->ptrs[0].batch_addr;
			   ),

		    TP_printk("dev=%s, fence=%p, seqno=%u, lrc_seqno=%u, gt=%u, guc_id=%d, batch_addr=0x%012llx, guc_state=0x%x, flags=0x%x, error=%d",
			      __get_str(dev), __entry->fence, __entry->seqno,
			      __entry->lrc_seqno, __entry->gt_id, __entry->guc_id,
			      __entry->batch_addr, __entry->guc_state,
			      __entry->flags, __entry->error)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_create,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_exec,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_run,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_free,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_timedout,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_set_error,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_ban,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DECLARE_EVENT_CLASS(xe_sched_msg,
		    TP_PROTO(struct xe_sched_msg *msg),
		    TP_ARGS(msg),

		    TP_STRUCT__entry(
			     __string(dev, __dev_name_eq(((struct xe_exec_queue *)msg->private_data)))
			     __field(u32, opcode)
			     __field(u16, guc_id)
			     __field(u8, gt_id)
			     ),

		    TP_fast_assign(
			   __assign_str(dev);
			   __entry->opcode = msg->opcode;
			   __entry->guc_id =
			   ((struct xe_exec_queue *)msg->private_data)->guc->id;
			   __entry->gt_id =
			   ((struct xe_exec_queue *)msg->private_data)->gt->info.id;
			   ),

		    TP_printk("dev=%s, gt=%u guc_id=%d, opcode=%u", __get_str(dev), __entry->gt_id, __entry->guc_id,
			      __entry->opcode)
);

DEFINE_EVENT(xe_sched_msg, xe_sched_msg_add,
	     TP_PROTO(struct xe_sched_msg *msg),
	     TP_ARGS(msg)
);

DEFINE_EVENT(xe_sched_msg, xe_sched_msg_recv,
	     TP_PROTO(struct xe_sched_msg *msg),
	     TP_ARGS(msg)
);

DECLARE_EVENT_CLASS(xe_hw_fence,
		    TP_PROTO(struct xe_hw_fence *fence),
		    TP_ARGS(fence),

		    TP_STRUCT__entry(
			     __string(dev, __dev_name_xe(fence->xe))
			     __field(u64, ctx)
			     __field(u32, seqno)
			     __field(struct xe_hw_fence *, fence)
			     ),

		    TP_fast_assign(
			   __assign_str(dev);
			   __entry->ctx = fence->dma.context;
			   __entry->seqno = fence->dma.seqno;
			   __entry->fence = fence;
			   ),

		    TP_printk("dev=%s, ctx=0x%016llx, fence=%p, seqno=%u",
			      __get_str(dev), __entry->ctx, __entry->fence, __entry->seqno)
);

DEFINE_EVENT(xe_hw_fence, xe_hw_fence_create,
	     TP_PROTO(struct xe_hw_fence *fence),
	     TP_ARGS(fence)
);

DEFINE_EVENT(xe_hw_fence, xe_hw_fence_signal,
	     TP_PROTO(struct xe_hw_fence *fence),
	     TP_ARGS(fence)
);

DEFINE_EVENT(xe_hw_fence, xe_hw_fence_try_signal,
	     TP_PROTO(struct xe_hw_fence *fence),
	     TP_ARGS(fence)
);

TRACE_EVENT(xe_reg_rw,
	TP_PROTO(struct xe_mmio *mmio, bool write, u32 reg, u64 val, int len),

	TP_ARGS(mmio, write, reg, val, len),

	TP_STRUCT__entry(
		__string(dev, __dev_name_tile(mmio->tile))
		__field(u64, val)
		__field(u32, reg)
		__field(u16, write)
		__field(u16, len)
		),

	TP_fast_assign(
		__assign_str(dev);
		__entry->val = val;
		__entry->reg = reg;
		__entry->write = write;
		__entry->len = len;
		),

	TP_printk("dev=%s, %s reg=0x%x, len=%d, val=(0x%x, 0x%x)",
		  __get_str(dev), __entry->write ? "write" : "read",
		  __entry->reg, __entry->len,
		  (u32)(__entry->val & 0xffffffff),
		  (u32)(__entry->val >> 32))
);

DECLARE_EVENT_CLASS(xe_pm_runtime,
		    TP_PROTO(struct xe_device *xe, void *caller),
		    TP_ARGS(xe, caller),

		    TP_STRUCT__entry(
			     __string(dev, __dev_name_xe(xe))
			     __field(void *, caller)
			     ),

		    TP_fast_assign(
			   __assign_str(dev);
			   __entry->caller = caller;
			   ),

		    TP_printk("dev=%s caller_function=%pS", __get_str(dev), __entry->caller)
);

DEFINE_EVENT(xe_pm_runtime, xe_pm_runtime_get,
	     TP_PROTO(struct xe_device *xe, void *caller),
	     TP_ARGS(xe, caller)
);

DEFINE_EVENT(xe_pm_runtime, xe_pm_runtime_put,
	     TP_PROTO(struct xe_device *xe, void *caller),
	     TP_ARGS(xe, caller)
);

DEFINE_EVENT(xe_pm_runtime, xe_pm_resume,
	     TP_PROTO(struct xe_device *xe, void *caller),
	     TP_ARGS(xe, caller)
);

DEFINE_EVENT(xe_pm_runtime, xe_pm_suspend,
	     TP_PROTO(struct xe_device *xe, void *caller),
	     TP_ARGS(xe, caller)
);

DEFINE_EVENT(xe_pm_runtime, xe_pm_runtime_resume,
	     TP_PROTO(struct xe_device *xe, void *caller),
	     TP_ARGS(xe, caller)
);

DEFINE_EVENT(xe_pm_runtime, xe_pm_runtime_suspend,
	     TP_PROTO(struct xe_device *xe, void *caller),
	     TP_ARGS(xe, caller)
);

DEFINE_EVENT(xe_pm_runtime, xe_pm_runtime_get_ioctl,
	     TP_PROTO(struct xe_device *xe, void *caller),
	     TP_ARGS(xe, caller)
);

TRACE_EVENT(xe_eu_stall_data_read,
	    TP_PROTO(u8 slice, u8 subslice,
		     u32 read_ptr, u32 write_ptr,
		     size_t read_size, size_t total_size),
	    TP_ARGS(slice, subslice,
		    read_ptr, write_ptr,
		    read_size, total_size),

	    TP_STRUCT__entry(__field(u8, slice)
			     __field(u8, subslice)
			     __field(u32, read_ptr)
			     __field(u32, write_ptr)
			     __field(size_t, read_size)
			     __field(size_t, total_size)
			     ),

	    TP_fast_assign(__entry->slice = slice;
			   __entry->subslice = subslice;
			   __entry->read_ptr = read_ptr;
			   __entry->write_ptr = write_ptr;
			   __entry->read_size = read_size;
			   __entry->total_size = total_size;
			   ),

	    TP_printk("slice: %u subslice: %u read ptr: 0x%x write ptr: 0x%x read size: %zu total read size: %zu",
		      __entry->slice, __entry->subslice,
		      __entry->read_ptr, __entry->write_ptr,
		      __entry->read_size, __entry->total_size)
);

TRACE_EVENT(xe_exec_queue_reach_max_job_count,
	    TP_PROTO(struct xe_exec_queue *q, int max_cnt),
	    TP_ARGS(q, max_cnt),

	    TP_STRUCT__entry(__string(dev, __dev_name_eq(q))
			     __field(enum xe_engine_class, class)
			     __field(u32, logical_mask)
			     __field(u16, guc_id)
			     __field(int, max_cnt)
			     ),

	    TP_fast_assign(__assign_str(dev);
			   __entry->class = q->class;
			   __entry->logical_mask = q->logical_mask;
			   __entry->guc_id = q->guc->id;
			   __entry->max_cnt = max_cnt;
			   ),

	    TP_printk("dev=%s, job count exceeded the maximum limit (%d) per exec queue. engine_class=0x%x, logical_mask=0x%x, guc_id=%d",
		      __get_str(dev), __entry->max_cnt,
		      __entry->class, __entry->logical_mask, __entry->guc_id)
);

#endif

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH MODULE_ABS_PATH(drivers/gpu/drm/xe)
#define TRACE_INCLUDE_FILE xe_trace
#include <trace/define_trace.h>
