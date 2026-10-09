// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_gt_debugfs.h"

#include <linux/debugfs.h>
#include <linux/ktime.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_managed.h>

#include "regs/xe_engine_regs.h"
#include "regs/xe_gtt_defs.h"
#include "regs/xe_gt_regs.h"

#include "xe_device.h"
#include "xe_force_wake.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_gt_mcr.h"
#include "xe_gt_idle.h"
#include "xe_gt_sriov_pf_debugfs.h"
#include "xe_gt_sriov_pf_helpers.h"
#include "xe_gt_sriov_vf_debugfs.h"
#include "xe_gt_stats.h"
#include "xe_gt_topology.h"
#include "xe_guc_hwconfig.h"
#include "xe_hw_engine.h"
#include "xe_lrc.h"
#include "xe_mocs.h"
#include "xe_mmio.h"
#include "xe_pat.h"
#include "xe_pm.h"
#include "xe_reg_sr.h"
#include "xe_reg_whitelist.h"
#include "xe_sa.h"
#include "xe_sriov.h"
#include "xe_sriov_vf_ccs.h"
#include "xe_tuning.h"
#include "xe_uc_debugfs.h"
#include "xe_wa.h"

static struct xe_gt *node_to_gt(struct drm_info_node *node)
{
	return node->dent->d_parent->d_inode->i_private;
}

/**
 * xe_gt_debugfs_simple_show - A show callback for struct drm_info_list
 * @m: the &seq_file
 * @data: data used by the drm debugfs helpers
 *
 * This callback can be used in struct drm_info_list to describe debugfs
 * files that are &xe_gt specific.
 *
 * It is assumed that those debugfs files will be created on directory entry
 * which struct dentry d_inode->i_private points to &xe_gt.
 *
 * This function assumes that &m->private will be set to the &struct
 * drm_info_node corresponding to the instance of the info on a given &struct
 * drm_minor (see struct drm_info_list.show for details).
 *
 * This function also assumes that struct drm_info_list.data will point to the
 * function code that will actually print a file content::
 *
 *   int (*print)(struct xe_gt *, struct drm_printer *)
 *
 * Example::
 *
 *    int foo(struct xe_gt *gt, struct drm_printer *p)
 *    {
 *        drm_printf(p, "GT%u\n", gt->info.id);
 *        return 0;
 *    }
 *
 *    static const struct drm_info_list bar[] = {
 *        { name = "foo", .show = xe_gt_debugfs_simple_show, .data = foo },
 *    };
 *
 *    dir = debugfs_create_dir("gt", parent);
 *    dir->d_inode->i_private = gt;
 *    drm_debugfs_create_files(bar, ARRAY_SIZE(bar), dir, minor);
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_debugfs_simple_show(struct seq_file *m, void *data)
{
	struct drm_printer p = drm_seq_file_printer(m);
	struct drm_info_node *node = m->private;
	struct xe_gt *gt = node_to_gt(node);
	int (*print)(struct xe_gt *, struct drm_printer *) = node->info_ent->data;

	if (WARN_ON(!print))
		return -EINVAL;

	return print(gt, &p);
}

/**
 * xe_gt_debugfs_show_with_rpm - A show callback for struct drm_info_list
 * @m: the &seq_file
 * @data: data used by the drm debugfs helpers
 *
 * Similar to xe_gt_debugfs_simple_show() but implicitly takes a RPM ref.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_debugfs_show_with_rpm(struct seq_file *m, void *data)
{
	struct drm_info_node *node = m->private;
	struct xe_gt *gt = node_to_gt(node);
	struct xe_device *xe = gt_to_xe(gt);

	guard(xe_pm_runtime)(xe);
	return xe_gt_debugfs_simple_show(m, data);
}

static int hw_engines(struct xe_gt *gt, struct drm_printer *p)
{
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FORCEWAKE_ALL);
	if (!xe_force_wake_ref_has_domain(fw_ref.domains, XE_FORCEWAKE_ALL))
		return -ETIMEDOUT;

	for_each_hw_engine(hwe, gt, id)
		xe_hw_engine_print(hwe, p);

	return 0;
}

static int steering(struct xe_gt *gt, struct drm_printer *p)
{
	xe_gt_mcr_steering_dump(gt, p);
	return 0;
}

/* Shared GT/engine programming, not a snapshot of a Windows VF context. */
static int mtl_sriov_cache_state(struct xe_gt *gt, struct drm_printer *p)
{
	static const struct {
		const char *name;
		struct xe_reg_mcr reg;
		u32 mask;
		u32 i915_expected;
	} regs[] = {
		{ "BLEND_FILL_CACHING_OPT_DIS", XEHP_L3SCQREG7,
		  BLEND_FILL_CACHING_OPT_DIS, BLEND_FILL_CACHING_OPT_DIS },
		{ "EN_32B_ACCESS", XEHP_SQCM, EN_32B_ACCESS, EN_32B_ACCESS },
		/* i915 STACKID_CTRL_512 = FIELD_PREP(GENMASK(6, 5), 2). */
		{ "STACKID_CTRL", RT_CTRL, REG_GENMASK(6, 5),
		  REG_FIELD_PREP(REG_GENMASK(6, 5), 2) },
		{ "INDIRECT_STATE_BASE_ADDR_OVERRIDE", SAMPLER_MODE,
		  INDIRECT_STATE_BASE_ADDR_OVERRIDE,
		  INDIRECT_STATE_BASE_ADDR_OVERRIDE },
		{ "DIS_FIX_EOT1_FLUSH", ROW_CHICKEN3,
		  DIS_FIX_EOT1_FLUSH, DIS_FIX_EOT1_FLUSH },
		{ "DISABLE_TDL_SVHS_GATING", ROW_CHICKEN2,
		  DISABLE_TDL_SVHS_GATING, DISABLE_TDL_SVHS_GATING },
		{ "FORCE_MISS_FTLB", COMP_MOD_CTRL,
		  FORCE_MISS_FTLB, FORCE_MISS_FTLB },
		{ "ENFORCE_RAR", SQCNT1, ENFORCE_RAR, ENFORCE_RAR },
	};
	struct xe_device *xe = gt_to_xe(gt);
	unsigned int i;

	if (!IS_SRIOV_PF(xe) || xe_gt_is_media_type(gt) ||
	    GRAPHICS_VERx100(xe) < 1270 || GRAPHICS_VERx100(xe) > 1274)
		return -ENODEV;

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FORCEWAKE_ALL);
	if (!xe_force_wake_ref_has_domain(fw_ref.domains, XE_FORCEWAKE_ALL))
		return -ETIMEDOUT;

	drm_printf(p, "gt=%u gfx=%u scope=shared-pf-gt-engine\n",
		   gt->info.id, GRAPHICS_VERx100(xe));
	drm_printf(p, "Sequential MCR reads; one valid instance per register.\n");
	drm_printf(p, "Not VF context state or cache contents.\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u32 value = xe_gt_mcr_unicast_read_any(gt, regs[i].reg);

		drm_printf(p, "%s reg=%#x val=%#x mask=%#x got=%#x i915=%#x match=%s\n",
			   regs[i].name, regs[i].reg.__reg.addr, value,
			   regs[i].mask, value & regs[i].mask,
			   regs[i].i915_expected,
			   str_yes_no((value & regs[i].mask) == regs[i].i915_expected));
	}

	return 0;
}

static int register_save_restore(struct xe_gt *gt, struct drm_printer *p)
{
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;

	xe_reg_sr_dump(&gt->reg_sr, p);
	drm_printf(p, "\n");

	drm_printf(p, "Engine\n");
	for_each_hw_engine(hwe, gt, id)
		xe_reg_sr_dump(&hwe->reg_sr, p);
	drm_printf(p, "\n");

	drm_printf(p, "LRC\n");
	for_each_hw_engine(hwe, gt, id)
		xe_reg_sr_dump(&hwe->reg_lrc, p);
	drm_printf(p, "\n");

	drm_printf(p, "Whitelist\n");
	for_each_hw_engine(hwe, gt, id) {
		xe_reg_whitelist_dump(&hwe->reg_whitelist, p);
		xe_reg_whitelist_dump(&hwe->oa_whitelist, p);
	}

	return 0;
}

struct mtl_sriov_render_identity {
	u32 lrca;
	u64 status;
	u64 pte;
	u32 range_vf;
};

static struct mtl_sriov_render_identity
mtl_sriov_render_identity(struct xe_hw_engine *hwe)
{
	struct xe_gt *gt = hwe->gt;
	struct xe_ggtt *ggtt = gt->tile->mem.ggtt;
	struct mtl_sriov_render_identity id = {};
	unsigned int n, total = xe_gt_sriov_pf_get_totalvfs(gt);

	id.lrca = xe_hw_engine_mmio_read32(hwe, RING_CURRENT_LRCA(0));
	id.status = xe_mmio_read64_2x32(&gt->mmio,
				      RING_EXECLIST_STATUS_LO(hwe->mmio_base));
	/* Zero may be power-gated/idle. Do not assign it to a VF or read page 0. */
	if (!id.lrca || !IS_ALIGNED(id.lrca, XE_PAGE_SIZE) || id.lrca >= GUC_GGTT_TOP)
		return id;

	/* Protect provisioning metadata, not hardware context switching. */
	guard(mutex)(xe_gt_sriov_pf_master_mutex(gt));
	for (n = 1; n <= total; n++) {
		const struct xe_ggtt_node *node = gt->sriov.pf.vfs[n].config.ggtt_region;
		u64 start, size;

		if (!node)
			continue;
		start = xe_ggtt_node_addr(node);
		size = xe_ggtt_node_size(node);
		if (id.lrca >= start && id.lrca - start < size) {
			id.range_vf = n;
			break;
		}
	}
	id.pte = xe_ggtt_read_pte(ggtt, id.lrca);
	return id;
}

static void mtl_sriov_render_identity_print(struct drm_printer *p,
					  const char *phase, struct xe_hw_engine *hwe,
					  const struct mtl_sriov_render_identity *id)
{
	drm_printf(p, "%s engine=%s lrca=%#x status=%#llx range_vf=%u ",
		   phase, hwe->name, id->lrca, id->status, id->range_vf);
	drm_printf(p, "pte=%#llx pte_present=%u pte_owner=%llu\n", id->pte,
		   !!(id->pte & XE_PAGE_PRESENT), u64_get_bits(id->pte, GGTT_PTE_VFID));
}

/* Observational bracket only: equality cannot exclude a switch away and back. */
static int mtl_sriov_render_state(struct xe_gt *gt, struct drm_printer *p)
{
	static const struct {
		const char *name;
		struct xe_reg_mcr reg;
		u32 mask;
	} regs[] = {
		{ "MSAA_OPTIMIZATION_REDUC_DISABLE", CACHE_MODE_1,
		  MSAA_OPTIMIZATION_REDUC_DISABLE },
		{ "FD_END_COLLECT", XEHP_PSS_CHICKEN, FD_END_COLLECT },
		{ "DISABLE_STATE_CACHE_PERF_FIX", XEHP_COMMON_SLICE_CHICKEN3,
		  DISABLE_STATE_CACHE_PERF_FIX },
		{ "L3_PWM_TIMER_INIT_VAL", XEHP_L3SQCREG5, L3_PWM_TIMER_INIT_VAL_MASK },
	};
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_hw_engine *rcs = &gt->hw_engines[XE_HW_ENGINE_RCS0];
	struct xe_hw_engine *bcs = &gt->hw_engines[XE_HW_ENGINE_BCS0];
	struct mtl_sriov_render_identity before[2], after[2];
	u32 values[ARRAY_SIZE(regs)], lncf[8], nonpriv[RING_MAX_NONPRIV_SLOTS];
	u64 start_ns, end_ns;
	unsigned int i;
	bool bracket_equal, owner_matches;

	if (!IS_SRIOV_PF(xe) || xe_gt_is_media_type(gt) ||
	    GRAPHICS_VERx100(xe) < 1270 || GRAPHICS_VERx100(xe) > 1274 ||
	    !xe_hw_engine_is_valid(rcs) || !xe_hw_engine_is_valid(bcs))
		return -ENODEV;

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FORCEWAKE_ALL);
	if (!xe_force_wake_ref_has_domain(fw_ref.domains, XE_FORCEWAKE_ALL))
		return -ETIMEDOUT;

	start_ns = ktime_get_ns();
	before[0] = mtl_sriov_render_identity(rcs);
	before[1] = mtl_sriov_render_identity(bcs);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		values[i] = xe_gt_mcr_unicast_read_any(gt, regs[i].reg);
	for (i = 0; i < ARRAY_SIZE(lncf); i++)
		lncf[i] = xe_gt_mcr_unicast_read_any(gt, XEHP_LNCFCMOCS(i));
	for (i = 0; i < ARRAY_SIZE(nonpriv); i++)
		nonpriv[i] = xe_hw_engine_mmio_read32(rcs, RING_FORCE_TO_NONPRIV(0, i));
	after[1] = mtl_sriov_render_identity(bcs);
	after[0] = mtl_sriov_render_identity(rcs);
	end_ns = ktime_get_ns();

	bracket_equal = before[0].lrca && before[0].lrca == after[0].lrca &&
		before[0].status == after[0].status && before[0].pte == after[0].pte &&
		before[0].range_vf == after[0].range_vf;
	owner_matches = before[0].range_vf && (before[0].pte & XE_PAGE_PRESENT) &&
		before[0].range_vf == u64_get_bits(before[0].pte, GGTT_PTE_VFID);
	drm_printf(p, "gt=%u gfx=%u start_ns=%llu end_ns=%llu ",
		   gt->info.id, GRAPHICS_VERx100(xe), start_ns, end_ns);
	drm_printf(p, "rcs_bracket_equal=%u rcs_owner_matches=%u\n",
		   bracket_equal, owner_matches);
	drm_puts(p, "Sequential reads, one valid MCR instance; no context pause.\n");
	drm_puts(p, "Equal brackets do not exclude an ABA switch or prove VF execution.\n");
	mtl_sriov_render_identity_print(p, "before", rcs, &before[0]);
	mtl_sriov_render_identity_print(p, "before", bcs, &before[1]);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		drm_printf(p, "%s reg=%#x val=%#x mask=%#x got=%#x\n",
			   regs[i].name, regs[i].reg.__reg.addr, values[i],
			   regs[i].mask, values[i] & regs[i].mask);
	for (i = 0; i < ARRAY_SIZE(lncf); i++)
		drm_printf(p, "LNCF%u reg=%#x val=%#x\n", i,
			   XEHP_LNCFCMOCS(i).__reg.addr, lncf[i]);
	for (i = 0; i < ARRAY_SIZE(nonpriv); i++)
		drm_printf(p, "NONPRIV%u reg=%#x val=%#x\n", i,
			   RING_FORCE_TO_NONPRIV(rcs->mmio_base, i).addr, nonpriv[i]);
	mtl_sriov_render_identity_print(p, "after", bcs, &after[1]);
	mtl_sriov_render_identity_print(p, "after", rcs, &after[0]);
	return 0;
}

/*
 * Check the registers referenced on a save-restore list and report any
 * save-restore entries that did not get applied.
 */
static int register_save_restore_check(struct xe_gt *gt, struct drm_printer *p)
{
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FORCEWAKE_ALL);
	if (!xe_force_wake_ref_has_domain(fw_ref.domains, XE_FORCEWAKE_ALL)) {
		drm_printf(p, "ERROR: Could not acquire forcewake\n");
		return -ETIMEDOUT;
	}

	xe_reg_sr_readback_check(&gt->reg_sr, gt, p);
	for_each_hw_engine(hwe, gt, id)
		xe_reg_sr_readback_check(&hwe->reg_sr, gt, p);
	for_each_hw_engine(hwe, gt, id)
		xe_reg_sr_lrc_check(&hwe->reg_lrc, gt, hwe, p);

	return 0;
}

static int rcs_default_lrc(struct xe_gt *gt, struct drm_printer *p)
{
	xe_lrc_dump_default(p, gt, XE_ENGINE_CLASS_RENDER);
	return 0;
}

static int ccs_default_lrc(struct xe_gt *gt, struct drm_printer *p)
{
	xe_lrc_dump_default(p, gt, XE_ENGINE_CLASS_COMPUTE);
	return 0;
}

static int bcs_default_lrc(struct xe_gt *gt, struct drm_printer *p)
{
	xe_lrc_dump_default(p, gt, XE_ENGINE_CLASS_COPY);
	return 0;
}

static int vcs_default_lrc(struct xe_gt *gt, struct drm_printer *p)
{
	xe_lrc_dump_default(p, gt, XE_ENGINE_CLASS_VIDEO_DECODE);
	return 0;
}

static int vecs_default_lrc(struct xe_gt *gt, struct drm_printer *p)
{
	xe_lrc_dump_default(p, gt, XE_ENGINE_CLASS_VIDEO_ENHANCE);
	return 0;
}

static int hwconfig(struct xe_gt *gt, struct drm_printer *p)
{
	xe_guc_hwconfig_dump(&gt->uc.guc, p);
	return 0;
}

/*
 * only for GT debugfs files which can be safely used on the VF as well:
 * - without access to the GT privileged registers
 * - without access to the PF specific data
 */
static const struct drm_info_list vf_safe_debugfs_list[] = {
	{ "topology", .show = xe_gt_debugfs_show_with_rpm, .data = xe_gt_topology_dump },
	{ "register-save-restore",
		.show = xe_gt_debugfs_show_with_rpm, .data = register_save_restore },
	{ "workarounds", .show = xe_gt_debugfs_show_with_rpm, .data = xe_wa_gt_dump },
	{ "tunings", .show = xe_gt_debugfs_show_with_rpm, .data = xe_tuning_dump },
	{ "default_lrc_rcs", .show = xe_gt_debugfs_show_with_rpm, .data = rcs_default_lrc },
	{ "default_lrc_ccs", .show = xe_gt_debugfs_show_with_rpm, .data = ccs_default_lrc },
	{ "default_lrc_bcs", .show = xe_gt_debugfs_show_with_rpm, .data = bcs_default_lrc },
	{ "default_lrc_vcs", .show = xe_gt_debugfs_show_with_rpm, .data = vcs_default_lrc },
	{ "default_lrc_vecs", .show = xe_gt_debugfs_show_with_rpm, .data = vecs_default_lrc },
	{ "hwconfig", .show = xe_gt_debugfs_show_with_rpm, .data = hwconfig },
	{ "pat_sw_config", .show = xe_gt_debugfs_simple_show, .data = xe_pat_dump_sw_config },
	{ "register-save-restore-check",
		.show = xe_gt_debugfs_show_with_rpm, .data = register_save_restore_check },
};

/* everything else should be added here */
static const struct drm_info_list pf_only_debugfs_list[] = {
	{ "mtl_sriov_render_state", .show = xe_gt_debugfs_show_with_rpm,
	  .data = mtl_sriov_render_state },
	{ "mtl_sriov_cache_state", .show = xe_gt_debugfs_show_with_rpm,
	  .data = mtl_sriov_cache_state },
	{ "hw_engines", .show = xe_gt_debugfs_show_with_rpm, .data = hw_engines },
	{ "mocs", .show = xe_gt_debugfs_show_with_rpm, .data = xe_mocs_dump },
	{ "pat", .show = xe_gt_debugfs_show_with_rpm, .data = xe_pat_dump },
	{ "powergate_info", .show = xe_gt_debugfs_show_with_rpm, .data = xe_gt_idle_pg_print },
	{ "steering", .show = xe_gt_debugfs_show_with_rpm, .data = steering },
};

static ssize_t write_to_gt_call(const char __user *userbuf, size_t count, loff_t *ppos,
				void (*call)(struct xe_gt *), struct xe_gt *gt)
{
	bool yes;
	int ret;

	if (*ppos)
		return -EINVAL;
	ret = kstrtobool_from_user(userbuf, count, &yes);
	if (ret < 0)
		return ret;
	if (yes)
		call(gt);
	return count;
}

static ssize_t stats_write(struct file *file, const char __user *userbuf,
			   size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct xe_gt *gt = s->private;

	return write_to_gt_call(userbuf, count, ppos, xe_gt_stats_clear, gt);
}

static int stats_show(struct seq_file *s, void *unused)
{
	struct drm_printer p = drm_seq_file_printer(s);
	struct xe_gt *gt = s->private;

	return xe_gt_stats_print_info(gt, &p);
}
DEFINE_SHOW_STORE_ATTRIBUTE(stats);

static void force_reset(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);

	guard(xe_pm_runtime)(xe);
	xe_gt_reset_async(gt);
}

static ssize_t force_reset_write(struct file *file,
				 const char __user *userbuf,
				 size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct xe_gt *gt = s->private;

	return write_to_gt_call(userbuf, count, ppos, force_reset, gt);
}

static int force_reset_show(struct seq_file *s, void *unused)
{
	struct xe_gt *gt = s->private;

	force_reset(gt); /* to be deprecated! */
	return 0;
}
DEFINE_SHOW_STORE_ATTRIBUTE(force_reset);

static void force_reset_sync(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);

	guard(xe_pm_runtime)(xe);
	xe_gt_reset(gt);
}

static ssize_t force_reset_sync_write(struct file *file,
				      const char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct xe_gt *gt = s->private;

	return write_to_gt_call(userbuf, count, ppos, force_reset_sync, gt);
}

static int force_reset_sync_show(struct seq_file *s, void *unused)
{
	struct xe_gt *gt = s->private;

	force_reset_sync(gt); /* to be deprecated! */
	return 0;
}
DEFINE_SHOW_STORE_ATTRIBUTE(force_reset_sync);

void xe_gt_debugfs_register(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct drm_minor *minor = gt_to_xe(gt)->drm.primary;
	struct dentry *parent = gt->tile->debugfs;
	struct dentry *root;
	char symlink[16];
	char name[8];

	xe_gt_assert(gt, minor->debugfs_root);

	if (IS_ERR(parent))
		return;

	snprintf(name, sizeof(name), "gt%d", gt->info.id);
	root = debugfs_create_dir(name, parent);
	if (IS_ERR(root)) {
		drm_warn(&xe->drm, "Create GT directory failed");
		return;
	}

	/*
	 * Store the xe_gt pointer as private data of the gt/ directory node
	 * so other GT specific attributes under that directory may refer to
	 * it by looking at its parent node private data.
	 */
	root->d_inode->i_private = gt;

	/* VF safe */
	debugfs_create_file("stats", 0600, root, gt, &stats_fops);
	debugfs_create_file("force_reset", 0600, root, gt, &force_reset_fops);
	debugfs_create_file("force_reset_sync", 0600, root, gt, &force_reset_sync_fops);

	drm_debugfs_create_files(vf_safe_debugfs_list,
				 ARRAY_SIZE(vf_safe_debugfs_list),
				 root, minor);

	if (!IS_SRIOV_VF(xe))
		drm_debugfs_create_files(pf_only_debugfs_list,
					 ARRAY_SIZE(pf_only_debugfs_list),
					 root, minor);

	xe_uc_debugfs_register(&gt->uc, root);

	if (IS_SRIOV_PF(xe))
		xe_gt_sriov_pf_debugfs_register(gt, root);
	else if (IS_SRIOV_VF(xe))
		xe_gt_sriov_vf_debugfs_register(gt, root);

	/*
	 * Backwards compatibility only: create a link for the legacy clients
	 * who may expect gt/ directory at the root level, not the tile level.
	 */
	snprintf(symlink, sizeof(symlink), "tile%u/%s", gt->tile->id, name);
	debugfs_create_symlink(name, minor->debugfs_root, symlink);
}
