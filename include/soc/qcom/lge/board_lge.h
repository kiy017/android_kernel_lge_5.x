/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stand-in for LG's <soc/qcom/lge/board_lge.h> (timelm port).
 *
 * The LG touch core only needs the touch-id enum and is_ddic_name(); the
 * boot-mode / LAF / factory helpers are only referenced under
 * CONFIG_LGE_USB_FACTORY / CONFIG_MACH_LGE, which this kernel does not set.
 */
#ifndef __ASM_ARCH_MSM_BOARD_LGE_H
#define __ASM_ARCH_MSM_BOARD_LGE_H

#include <linux/types.h>

typedef enum {
	TOUCH_NORMAL = 0,
	SYNAPTICS = 8,
	SIW = 9,
} lge_touch_id_t;

lge_touch_id_t lge_get_touch_id(void);
bool is_ddic_name(char *ddic_name);

#endif
