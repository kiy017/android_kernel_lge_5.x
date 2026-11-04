// SPDX-License-Identifier: GPL-2.0-only
/*
 * Board-info helpers the LG touch core expects from LG's board_lge /
 * lge_dsi_panel code, which the 5.15 BSP does not have (timelm port).
 */

#include <linux/string.h>
#include <soc/qcom/lge/board_lge.h>

/* timelm ships one panel: the SW43103 DDIC paired with the SW42902 touch IC. */
bool is_ddic_name(char *ddic_name)
{
	return ddic_name && !strcmp(ddic_name, "sw43103");
}

lge_touch_id_t lge_get_touch_id(void)
{
	return SIW;
}
