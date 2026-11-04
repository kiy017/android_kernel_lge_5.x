/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TIMELM_SUBSYS_COMPAT_H
#define __TIMELM_SUBSYS_COMPAT_H

struct rproc;

#if IS_ENABLED(CONFIG_TIMELM_SUBSYS_COMPAT)
int timelm_subsys_compat_add(struct rproc *rproc, const char *name);
#else
static inline int timelm_subsys_compat_add(struct rproc *rproc,
					   const char *name)
{
	return 0;
}
#endif

#endif /* __TIMELM_SUBSYS_COMPAT_H */
