// SPDX-License-Identifier: GPL-2.0-only
/*
 * DEBUG(timelm): keep the SDX55 DIAG stream in memory, there is no diag_mdlog
 * on this ROM. Asks the MDM for F3 messages and events while the channel is
 * open and appends all it sends to /sys/kernel/debug/timelm_mdmlog.
 */

#include <linux/debugfs.h>
#include <linux/diagchar.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include "diagchar.h"
#include "diagchar_hdlc.h"
#include "diagfwd_bridge.h"
#include "timelm_mdmlog.h"

#define TIMELM_MDMLOG_SIZE	(64 * SZ_1M)
#define TIMELM_MDMLOG_MAGIC	0x4d444d54	/* "TMDM" */
#define TIMELM_MDMLOG_TICK_MS	2000
/* Peripherals behind the MDM come up late, so send for about a minute. */
#define TIMELM_MDMLOG_TICKS	30
#define TIMELM_MDMLOG_MAX_CMD	8

enum timelm_mdmlog_type {
	TIMELM_MDMLOG_RX,
	TIMELM_MDMLOG_TX,
	TIMELM_MDMLOG_OPEN,
	TIMELM_MDMLOG_CLOSE,
};

struct timelm_mdmlog_rec {
	u32 magic;
	u32 type;
	u64 ns;
	u32 len;
	u32 rc;
} __packed;

static const u8 timelm_mdmlog_cmds[][TIMELM_MDMLOG_MAX_CMD] = {
	/* Liveness: the build id answer shows the MDM parses our commands. */
	{ DIAG_CMD_EXT_BUILD },
	/* All SSIDs, medium level and up. */
	{ DIAG_CMD_MSG_CONFIG, DIAG_CMD_OP_SET_ALL_MSG_MASK, 0, 0, MSG_LVL_MED },
	{ DIAG_CMD_EVENT_TOGGLE, 1 },
};
static const u8 timelm_mdmlog_cmd_len[] = { 1, 8, 2 };

/* Written once and never changed, so overlapping writes of a frame are fine. */
static u8 *timelm_mdmlog_frame[ARRAY_SIZE(timelm_mdmlog_cmds)];
static int timelm_mdmlog_frame_len[ARRAY_SIZE(timelm_mdmlog_cmds)];

static u8 *timelm_mdmlog_buf;
static size_t timelm_mdmlog_used;
static bool timelm_mdmlog_full;
static DEFINE_SPINLOCK(timelm_mdmlog_lock);
static int timelm_mdmlog_ticks_left;

static void timelm_mdmlog_add_rc(enum timelm_mdmlog_type type, const void *data,
				 u32 len, u32 rc)
{
	struct timelm_mdmlog_rec rec = {
		.magic = TIMELM_MDMLOG_MAGIC,
		.type = type,
		.ns = ktime_get_boottime_ns(),
		.len = len,
		.rc = rc,
	};
	size_t used, need = sizeof(rec) + len;
	unsigned long flags;
	bool now_full = false;

	if (!timelm_mdmlog_buf)
		return;

	spin_lock_irqsave(&timelm_mdmlog_lock, flags);
	used = timelm_mdmlog_used;
	if (used + need > TIMELM_MDMLOG_SIZE) {
		now_full = !timelm_mdmlog_full;
		timelm_mdmlog_full = true;
	} else {
		memcpy(timelm_mdmlog_buf + used, &rec, sizeof(rec));
		if (len)
			memcpy(timelm_mdmlog_buf + used + sizeof(rec), data, len);
		/* Pairs with the acquire in the reader, which takes no lock. */
		smp_store_release(&timelm_mdmlog_used, used + need);
	}
	spin_unlock_irqrestore(&timelm_mdmlog_lock, flags);

	if (now_full)
		pr_err("TIMELMDIAG: capture full at %d MB, dropping the rest\n",
		       TIMELM_MDMLOG_SIZE / SZ_1M);
}

void timelm_mdmlog_add(const void *data, int len)
{
	if (len > 0)
		timelm_mdmlog_add_rc(TIMELM_MDMLOG_RX, data, len, 0);
}

static void timelm_mdmlog_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(timelm_mdmlog_work, timelm_mdmlog_work_fn);

static void timelm_mdmlog_work_fn(struct work_struct *work)
{
	int i, err;

	for (i = 0; i < ARRAY_SIZE(timelm_mdmlog_frame); i++) {
		if (!timelm_mdmlog_frame[i])
			continue;
		err = diagfwd_bridge_write(DIAGFWD_MDM, timelm_mdmlog_frame[i],
					   timelm_mdmlog_frame_len[i]);
		timelm_mdmlog_add_rc(TIMELM_MDMLOG_TX, timelm_mdmlog_frame[i],
				     timelm_mdmlog_frame_len[i], err);
		if (err)
			pr_err_ratelimited("TIMELMDIAG: cmd %d write failed: %d\n",
					   i, err);
	}

	if (--timelm_mdmlog_ticks_left > 0)
		schedule_delayed_work(&timelm_mdmlog_work,
				      msecs_to_jiffies(TIMELM_MDMLOG_TICK_MS));
}

void timelm_mdmlog_open(void)
{
	timelm_mdmlog_add_rc(TIMELM_MDMLOG_OPEN, NULL, 0, 0);
	/* A crash recovery opens the channel again: ask again, from the start. */
	timelm_mdmlog_ticks_left = TIMELM_MDMLOG_TICKS;
	mod_delayed_work(system_wq, &timelm_mdmlog_work, 0);
	pr_info("TIMELMDIAG: MDM channel open, sending masks\n");
}

void timelm_mdmlog_close(void)
{
	timelm_mdmlog_add_rc(TIMELM_MDMLOG_CLOSE, NULL, 0, 0);
	cancel_delayed_work(&timelm_mdmlog_work);
}

static ssize_t timelm_mdmlog_read(struct file *file, char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, count, ppos, timelm_mdmlog_buf,
				       smp_load_acquire(&timelm_mdmlog_used));
}

static const struct file_operations timelm_mdmlog_fops = {
	.read = timelm_mdmlog_read,
	.open = simple_open,
	.llseek = default_llseek,
};

void timelm_mdmlog_init(void)
{
	struct diag_send_desc_type send = { NULL, NULL, DIAG_STATE_START, 0 };
	struct diag_hdlc_dest_type enc = { NULL, NULL, 0 };
	int i, len, max_len;

	timelm_mdmlog_buf = vmalloc(TIMELM_MDMLOG_SIZE);
	if (!timelm_mdmlog_buf) {
		pr_err("TIMELMDIAG: no memory for the capture\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(timelm_mdmlog_cmds); i++) {
		len = timelm_mdmlog_cmd_len[i];
		/* Worst case every byte escaped, plus CRC and delimiter. */
		max_len = (2 * len) + 3 + 2;
		/* kmalloc: MHI maps these for DMA. */
		timelm_mdmlog_frame[i] = kmalloc(max_len, GFP_KERNEL);
		if (!timelm_mdmlog_frame[i])
			continue;
		send.state = DIAG_STATE_START;
		send.pkt = timelm_mdmlog_cmds[i];
		send.last = timelm_mdmlog_cmds[i] + len - 1;
		send.terminate = 1;
		enc.dest = timelm_mdmlog_frame[i];
		enc.dest_last = timelm_mdmlog_frame[i] + max_len - 1;
		diag_hdlc_encode(&send, &enc);
		timelm_mdmlog_frame_len[i] = (u8 *)enc.dest - timelm_mdmlog_frame[i];
	}

	debugfs_create_file("timelm_mdmlog", 0444, NULL, NULL, &timelm_mdmlog_fops);
}
