// SPDX-License-Identifier: GPL-2.0-only
/*
 * schedtune cgroup controller stub - LG V60 (timelm) 5.15 bring-up
 *
 * LG's Android 16 userspace still expects the CAF "schedtune" cgroup v1
 * hierarchy: libprocessgroup's SetupCgroups() mounts every controller listed
 * in /system/etc/cgroups.json, and schedtune is one of them.  The msm-5.15
 * consolidated BSP dropped CAF's kernel/sched/tune.c (schedutil/uclamp is
 * what it uses instead), so mounting schedtune fails with EINVAL.  Because
 * that makes the whole SetupCgroups builtin fail, /sys/fs/cgroup/system is
 * never created and *no* service can be started:
 *
 *   cgroup: Unknown subsys name 'schedtune'
 *   libprocessgroup: Failed to setup schedtune cgroup
 *   init: Command 'SetupCgroups' ... failed: Invalid argument
 *   libprocessgroup: Failed to make and chown /sys/fs/cgroup/system/uid_0
 *   init: Service 'ueventd' failed to start due to a fatal error
 *   ... Service 'apexd-bootstrap' failed to start due to a fatal error
 *   init: Got shutdown_command 'reboot,bootloader,bootstrap-apexd-failed'
 *
 * This file registers the controller (mountable, with the same attribute
 * names LG's task_profiles writes to) but stores the values as no-ops.  It
 * exists purely so the cgroup hierarchy can be set up; it does not implement
 * boost semantics.  If we ever want real schedtune boosting, the 4.19
 * implementation to port from is
 * android_kernel_lge_sm8250/kernel/sched/tune.c.
 */

#include <linux/cgroup.h>
#include <linux/slab.h>

struct schedtune {
	struct cgroup_subsys_state css;
	s64 boost;
	u64 prefer_idle;
	u64 colocate;
	u64 boost_no_override;
};

static struct schedtune *css_st(struct cgroup_subsys_state *css)
{
	return container_of(css, struct schedtune, css);
}

static struct cgroup_subsys_state *
schedtune_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct schedtune *st;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return ERR_PTR(-ENOMEM);

	/* Values the ROM's task_profiles apply by default. */
	st->boost = 0;
	st->prefer_idle = 0;
	st->colocate = 0;
	st->boost_no_override = 0;

	return &st->css;
}

static void schedtune_css_free(struct cgroup_subsys_state *css)
{
	kfree(css_st(css));
}

static s64 boost_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return css_st(css)->boost;
}

static int boost_write(struct cgroup_subsys_state *css, struct cftype *cft,
		       s64 val)
{
	css_st(css)->boost = val;
	return 0;
}

static u64 prefer_idle_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return css_st(css)->prefer_idle;
}

static int prefer_idle_write(struct cgroup_subsys_state *css,
			     struct cftype *cft, u64 val)
{
	css_st(css)->prefer_idle = val;
	return 0;
}

static u64 colocate_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return css_st(css)->colocate;
}

static int colocate_write(struct cgroup_subsys_state *css, struct cftype *cft,
			  u64 val)
{
	css_st(css)->colocate = val;
	return 0;
}

static u64 boost_no_override_read(struct cgroup_subsys_state *css,
				  struct cftype *cft)
{
	return css_st(css)->boost_no_override;
}

static int boost_no_override_write(struct cgroup_subsys_state *css,
				   struct cftype *cft, u64 val)
{
	css_st(css)->boost_no_override = val;
	return 0;
}

static struct cftype files[] = {
	{
		.name = "boost",
		.read_s64 = boost_read,
		.write_s64 = boost_write,
	},
	{
		.name = "prefer_idle",
		.read_u64 = prefer_idle_read,
		.write_u64 = prefer_idle_write,
	},
	{
		.name = "colocate",
		.read_u64 = colocate_read,
		.write_u64 = colocate_write,
	},
	{
		.name = "sched_boost_no_override",
		.read_u64 = boost_no_override_read,
		.write_u64 = boost_no_override_write,
	},
	{ }	/* terminate */
};

/*
 * NOTE: no .early_init here on purpose.
 *
 * With .early_init = 1 the core calls css_alloc() from cgroup_init_early(),
 * which runs in start_kernel() *before* mm_init() - i.e. before the slab
 * allocator exists.  schedtune_css_alloc()'s kzalloc() then dereferences an
 * uninitialised allocator and the kernel dies on the spot, long before any
 * console (ramoops included) is registered: black boot, LG logo, reset, and
 * absolutely nothing in pstore.  That is exactly what img28 and img29 did.
 *
 * Without the flag the core initialises the controller from cgroup_init(),
 * after mm_init(), which is where every real controller allocates its root
 * css as well.
 */
struct cgroup_subsys schedtune_cgrp_subsys = {
	.css_alloc	= schedtune_css_alloc,
	.css_free	= schedtune_css_free,
	.legacy_cftypes	= files,
};
