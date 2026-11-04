// SPDX-License-Identifier: GPL-2.0-only
/*
 * LGE panel event notifier chain.
 *
 * On the stock LG kernel this lives in the LG display techpack
 * (msm/lge/notifier).  The 5.15 BSP display driver has no LG layer, so the
 * chain is provided here, next to its only client (the LG touch core), and
 * dsi_panel.c calls lge_panel_notifier_call_chain() at the same points the
 * stock lge_dsi_panel.c did (reset high/low, unblank, blank).
 */

#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/lge_panel_notify.h>

static BLOCKING_NOTIFIER_HEAD(lge_panel_notifier_list);

int lge_panel_notifier_register_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&lge_panel_notifier_list, nb);
}
EXPORT_SYMBOL(lge_panel_notifier_register_client);

int lge_panel_notifier_unregister_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&lge_panel_notifier_list, nb);
}
EXPORT_SYMBOL(lge_panel_notifier_unregister_client);

int lge_panel_notifier_call_chain(unsigned long val, int display_id, int state)
{
	struct lge_panel_notifier notifier_data = {
		.display_id = display_id,
		.state = state,
	};

	return blocking_notifier_call_chain(&lge_panel_notifier_list, val,
					    &notifier_data);
}
EXPORT_SYMBOL(lge_panel_notifier_call_chain);
