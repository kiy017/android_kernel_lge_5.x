// SPDX-License-Identifier: GPL-2.0-only
/*
 * DEBUG(timelm): timestamped trace of what the SDX55 and the nameservice
 * exchange, in /sys/kernel/debug/timelm_qrtrlog. The qrtr_ns ipc log is a small
 * ring that client lookups overwrite within seconds of a stalled boot.
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include "qrtr.h"

#define TIMELM_QRTRLOG_SIZE	(4 * SZ_1M)

static char timelm_qrtrlog_buf[TIMELM_QRTRLOG_SIZE];
static size_t timelm_qrtrlog_used;
static DEFINE_SPINLOCK(timelm_qrtrlog_lock);

void timelm_qrtrlog(const char *fmt, ...)
{
	u64 ns = ktime_get_boottime_ns();
	unsigned long flags;
	va_list args;
	size_t used;
	u32 rem;
	u64 sec;

	sec = div_u64_rem(ns, NSEC_PER_SEC, &rem);

	spin_lock_irqsave(&timelm_qrtrlog_lock, flags);
	used = timelm_qrtrlog_used;
	/* Full: keep the start of the boot, it is what we are after. */
	if (TIMELM_QRTRLOG_SIZE - used >= 256) {
		used += scnprintf(timelm_qrtrlog_buf + used, 32, "%llu.%06u ", sec,
				  rem / NSEC_PER_USEC);
		va_start(args, fmt);
		used += vscnprintf(timelm_qrtrlog_buf + used, 224, fmt, args);
		va_end(args);
		/* Pairs with the acquire in the reader, which takes no lock. */
		smp_store_release(&timelm_qrtrlog_used, used);
	}
	spin_unlock_irqrestore(&timelm_qrtrlog_lock, flags);
}
EXPORT_SYMBOL_GPL(timelm_qrtrlog);

static ssize_t timelm_qrtrlog_read(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, count, ppos, timelm_qrtrlog_buf,
				       smp_load_acquire(&timelm_qrtrlog_used));
}

static const struct file_operations timelm_qrtrlog_fops = {
	.read = timelm_qrtrlog_read,
	.open = simple_open,
	.llseek = default_llseek,
};

static int __init timelm_qrtrlog_init(void)
{
	debugfs_create_file("timelm_qrtrlog", 0444, NULL, NULL,
			    &timelm_qrtrlog_fops);
	return 0;
}
late_initcall(timelm_qrtrlog_init);
