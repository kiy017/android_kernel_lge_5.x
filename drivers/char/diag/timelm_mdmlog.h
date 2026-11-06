/* SPDX-License-Identifier: GPL-2.0-only */
/* DEBUG(timelm): in-memory capture of the SDX55 DIAG stream. */

#ifndef TIMELM_MDMLOG_H
#define TIMELM_MDMLOG_H

void timelm_mdmlog_init(void);
void timelm_mdmlog_open(void);
void timelm_mdmlog_close(void);
void timelm_mdmlog_add(const void *data, int len);

#endif
