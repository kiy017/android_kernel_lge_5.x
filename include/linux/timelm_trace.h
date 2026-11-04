/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TMPDBG (timelm): comm filter + payload dumper for the temporary vendor TEE
 * daemon tracing.
 *
 * img36 (log21) showed that vendor.qseecomd dies 2.4 ms after opendir("/dev"),
 * without ever opening /dev/qseecom, and that keymaster-4-0 produced *no*
 * trace at all - its comm is the basename of
 * /vendor/bin/hw/android.hardware.keymaster@4.0-service-qti truncated to
 * TASK_COMM_LEN-1, i.e. "android.hardwar", not "keymaster-4-0".
 *
 * Both daemons report their failures through liblog (logd socket) and LG's
 * qseecomd also writes to /dev/pmsg0, neither of which shows up in the
 * ramoops console ring we read back.  So dump the first bytes of everything
 * they write, with non-printables folded to '.', straight into the kernel
 * log.
 *
 * Remove together with the call sites in fs/open.c, fs/ioctl.c,
 * fs/read_write.c, net/socket.c.
 */
#ifndef _LINUX_TIMELM_TRACE_H
#define _LINUX_TIMELM_TRACE_H

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>

#ifdef CONFIG_TIMELM_TRACE

/* "android.hardware.*" HALs are all comm "android.hardwar" (15 bytes) */
#define TIMELM_TRACE_HAL_PREFIX	"android.hardware"
#define TIMELM_TRACE_HAL_LEN	15

/*
 * Master switch.  The TEE stack (qseecom + ION + keymaster) works now, and
 * this tracing costs ~2400 of the ~2500 lines a 256 KB ramoops console holds
 * per boot, i.e. it had shrunk the captured window to 3.5 s and dropped the
 * whole boot history.  Keep it compiled in, flip to true when chasing vendor
 * userspace again.
 */
#define TIMELM_TRACE_ENABLED	false

static inline bool timelm_trace_comm(void)
{
	if (!TIMELM_TRACE_ENABLED)
		return false;

	/* comm is TASK_COMM_LEN (16) bytes, including the NUL */
	return !strcmp(current->comm, "qseecomd") ||
	       !strcmp(current->comm, "qseecom-servic") ||
	       !strcmp(current->comm, "keystore2") ||
	       !strncmp(current->comm, TIMELM_TRACE_HAL_PREFIX,
			TIMELM_TRACE_HAL_LEN);
}

/*
 * Payload dumping: qseecomd (its binary status records) and now also the TEE
 * HALs.  keymaster-4-0 (pid 369 in img25/log25) gets through qseecom with
 * every ioctl returning success, never publishes its HIDL service and just
 * sits in futex waits - and everything it has to say about that goes to
 * liblog over /dev/socket/logdw, which is invisible in the ramoops console.
 */
/*
 * img143: the payload dump alone is back on (the failed-syscall/open/ioctl
 * tracing above stays off) to catch why keymaster's begin() fails with
 * INVALID_ARGUMENT on the VM and SoftBank phones: liblog of qseecomd (librpmb,
 * libssd) and the keymaster HAL.  Not every "android.hardwar" HAL nor
 * keystore2 (img143: 349 lines of startup chatter), they flood the 256 KB
 * console.
 */
#define TIMELM_TRACE_WRITE_ENABLED	true

static inline bool timelm_trace_comm_write(void)
{
	if (!TIMELM_TRACE_WRITE_ENABLED)
		return false;

	return !strcmp(current->comm, "qseecomd") ||
	       !strcmp(current->comm, "qseecom-servic") ||
	       !strncmp(current->comm, "keymaster@", 10);
}

#define TIMELM_TRACE_DUMP_MAX	200

static inline void timelm_trace_dump(int fd, const char __user *ubuf,
				     size_t count, const char *what)
{
	char *kbuf;
	size_t n, i;

	if (!timelm_trace_comm_write() || !count)
		return;

	n = count > TIMELM_TRACE_DUMP_MAX ? TIMELM_TRACE_DUMP_MAX : count;
	kbuf = kmalloc(n + 1, GFP_KERNEL);
	if (!kbuf)
		return;
	if (copy_from_user(kbuf, ubuf, n)) {
		kfree(kbuf);
		return;
	}
	for (i = 0; i < n; i++)
		if (kbuf[i] < 0x20 || kbuf[i] > 0x7e)
			kbuf[i] = '.';
	kbuf[n] = '\0';
	pr_info("TIMELMTRACE: %s %s fd=%d len=%zu: %s\n", current->comm, what,
		fd, count, kbuf);
	kfree(kbuf);
}

/*
 * writev/sendmsg style: dump the payload of the first four segments (liblog
 * to logdw is header, prio, tag, message)
 */
static inline void timelm_trace_dump_iov(int fd, const struct iovec __user *vec,
					 unsigned long vlen)
{
	struct iovec iov[4];
	unsigned long n, i;

	if (!timelm_trace_comm_write() || !vlen)
		return;

	n = vlen > ARRAY_SIZE(iov) ? ARRAY_SIZE(iov) : vlen;
	if (copy_from_user(iov, vec, n * sizeof(struct iovec)))
		return;
	for (i = 0; i < n; i++)
		timelm_trace_dump(fd, iov[i].iov_base, iov[i].iov_len, "writev");
}

/*
 * qseecomd exits -1 with every traced open/ioctl/connect successful, so the
 * decision is made on a syscall we do not instrument (faccessat, stat,
 * chmod/chown, capset, bind, socketpair, ...).  Log every failing syscall of
 * the traced processes, decoding the pathname argument where there is one.
 */
static inline void timelm_trace_failed_syscall(unsigned int nr, unsigned long a0,
					       unsigned long a1,
					       unsigned long a2, long ret)
{
	char pbuf[128];
	long n = -1;

	if (!timelm_trace_comm())
		return;

	/*
	 * android.hardwar (the TEE HALs) runs a "close every fd from 0 to 1023"
	 * loop at startup, which produced 1841 of the 2499 lines in one ring
	 * dump and pushed the interesting early boot out of the 256 KB ramoops
	 * console.  EBADF on close() is expected there and never informative.
	 */
	if (nr == __NR_close && ret == -EBADF)
		return;

	switch (nr) {
	case __NR_mkdirat:	/* (dfd, path, mode) */
	case __NR_faccessat:	/* (dfd, path, mode) */
	case __NR_faccessat2:	/* (dfd, path, mode, flags) */
	case __NR_fchmodat:	/* (dfd, path, mode) */
	case __NR_fchownat:	/* (dfd, path, uid, gid, flags) */
	case __NR_readlinkat:	/* (dfd, path, buf, size) */
	case __NR_newfstatat:	/* (dfd, path, statbuf, flags) */
	case __NR_statx:	/* (dfd, path, flags, mask, buf) */
		n = strncpy_from_user(pbuf, (const char __user *)a1,
				      sizeof(pbuf));
		break;
	default:
		break;
	}

	if (n >= 0)
		pr_info("SYSRET: %s nr=%u a0=0x%lx a1=0x%lx a2=0x%lx -> %ld path=%s\n",
			current->comm, nr, a0, a1, a2, ret, pbuf);
	else
		pr_info("SYSRET: %s nr=%u a0=0x%lx a1=0x%lx a2=0x%lx -> %ld\n",
			current->comm, nr, a0, a1, a2, ret);
}

#else /* !CONFIG_TIMELM_TRACE */

static inline bool timelm_trace_comm(void) { return false; }
static inline void timelm_trace_dump(int fd, const char __user *ubuf,
				     size_t count, const char *what) { }
static inline void timelm_trace_dump_iov(int fd, const struct iovec __user *vec,
					 unsigned long vlen) { }
static inline void timelm_trace_failed_syscall(unsigned int nr, unsigned long a0,
					       unsigned long a1,
					       unsigned long a2, long ret) { }

#endif /* CONFIG_TIMELM_TRACE */

#endif /* _LINUX_TIMELM_TRACE_H */
