// SPDX-License-Identifier: GPL-2.0-only
/*
 * TMPDBG (timelm): dump a few /vendor files and directories into the
 * kernel log.
 *
 * vendor.qseecomd (LG TEE listener daemon) exits -1 2.4 ms after opendir("/dev")
 * without ever opening /dev/qseecom, and it logs only binary blobs to logd and
 * /dev/pmsg0, so we still do not know which device/property it is unhappy
 * about.  Its own rc file, the vendor property contexts and the strings in the
 * binary answer that, and a workqueue can read them from the mounted /vendor
 * long before userspace is reachable (no shell, no adb in this boot).
 *
 * Debug only - remove with the rest of the debug strip.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/delay.h>

static char *ul_read(const char *path, size_t max, loff_t *out_len)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t ret;
	char *buf;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_info("ULDUMP: open %s -> %ld\n", path, PTR_ERR(f));
		return NULL;
	}

	buf = vmalloc(max + 1);
	if (!buf) {
		fput(f);
		return NULL;
	}

	ret = kernel_read(f, buf, max, &pos);
	fput(f);
	if (ret < 0) {
		pr_info("ULDUMP: read %s -> %zd\n", path, ret);
		vfree(buf);
		return NULL;
	}
	buf[ret] = '\0';
	*out_len = ret;
	return buf;
}

/* print every line of a text file that contains @needle (NULL: all lines) */
static void ul_dump_text(const char *path, const char *needle, int maxlines)
{
	char *buf;
	loff_t len;
	long i = 0;
	int n = 0;

	buf = ul_read(path, 128 * 1024, &len);
	if (!buf)
		return;

	pr_info("ULDUMP: === %s (%lld bytes) ===\n", path, len);
	while (i < len && n < maxlines) {
		long j = i;

		while (j < len && buf[j] != '\n')
			j++;
		buf[j] = '\0';
		if (j > i && (!needle || strstr(buf + i, needle))) {
			pr_info("ULDUMP: %s\n", buf + i);
			n++;
		}
		i = j + 1;
	}
	vfree(buf);
}

/* substring search on a non NUL terminated buffer (strnstr is too subtle) */
static bool ul_name_has(const char *name, int len, const char *needle)
{
	int nl = strlen(needle);
	int i;

	for (i = 0; i + nl <= len; i++)
		if (!memcmp(name + i, needle, nl))
			return true;
	return false;
}

struct ul_dir_ctx {
	struct dir_context ctx;
	const char *path;
	const char *needle;
	int n;
	int shown;
	int max;
};

static int ul_dir_actor(struct dir_context *ctx, const char *name, int namlen,
			loff_t off, u64 ino, unsigned d_type)
{
	struct ul_dir_ctx *d = container_of(ctx, struct ul_dir_ctx, ctx);
	char tmp[64];

	if (!d->needle) {
		if (d->shown < d->max) {
			pr_info("ULDIR: %s/%.*s (type %u)\n", d->path, namlen,
				name, d_type);
			d->shown++;
		}
	} else {
		int l = namlen < (int)sizeof(tmp) - 1 ? namlen
						     : (int)sizeof(tmp) - 1;

		memcpy(tmp, name, l);
		tmp[l] = '\0';
		if (ul_name_has(name, namlen, d->needle) && d->shown < d->max) {
			pr_info("ULDIR: %s/%.*s (type %u)\n", d->path, namlen,
				name, d_type);
			d->shown++;
		}
	}
	d->n++;
	return 0;
}

static void ul_list_dir(const char *path, const char *needle, int max)
{
	struct ul_dir_ctx d = {
		.ctx.actor = ul_dir_actor,
		.path = path,
		.needle = needle,
		.max = max,
	};
	struct file *f;
	int ret;

	f = filp_open(path, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(f)) {
		pr_info("ULDIR: %s -> open error %ld\n", path, PTR_ERR(f));
		return;
	}
	ret = iterate_dir(f, &d.ctx);
	pr_info("ULDIR: %s -> %d entries, %d listed%s%s (ret %d)\n", path,
		d.n, d.shown, needle ? ", needle " : "",
		needle ? needle : "", ret);
	fput(f);
}

/* print printable strings >= 5 chars that contain @needle */
static void ul_dump_strings(const char *path, const char *needle, int maxprint)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t len;
	char *buf;
	long i = 0;
	int printed = 0;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_info("ULSTR: open %s -> %ld\n", path, PTR_ERR(f));
		return;
	}
	buf = vmalloc(2 * 1024 * 1024);
	if (!buf) {
		fput(f);
		return;
	}
	len = kernel_read(f, buf, 2 * 1024 * 1024, &pos);
	fput(f);
	if (len <= 0) {
		pr_info("ULSTR: read %s -> %zd\n", path, len);
		vfree(buf);
		return;
	}
	pr_info("ULDUMP: === strings %s (%zd bytes) matching '%s' ===\n",
		path, len, needle);
	while (i < len && printed < maxprint) {
		long j = i;
		int l;

		while (j < len && buf[j] >= 0x20 && buf[j] <= 0x7e)
			j++;
		l = j - i;
		if (l >= 5 && l < 200 && ul_name_has(buf + i, l, needle)) {
			char tmp[208];

			memcpy(tmp, buf + i, l);
			tmp[l] = '\0';
			pr_info("ULSTR: %s\n", tmp);
			printed++;
		}
		i = j + 1;
	}
	pr_info("ULDUMP: === strings %s: %d printed ===\n", path, printed);
	vfree(buf);
}

static void ul_exists(const char *path)
{
	struct file *f = filp_open(path, O_RDONLY, 0);

	if (IS_ERR(f)) {
		pr_info("ULFILE: %s -> %ld\n", path, PTR_ERR(f));
		return;
	}
	pr_info("ULFILE: %s -> present, %lld bytes\n", path,
		(long long)i_size_read(file_inode(f)));
	fput(f);
}

static void ul_dump_work(struct work_struct *work)
{
	pr_info("ULDUMP: start\n");

	/* is the modem partition (where the TEE apps live) mounted, and how? */
	ul_dump_text("/proc/mounts", "firmware", 6);
	/* librpmb reads this to find the secure partitions */
	ul_dump_text("/system/etc/rpmb_sec_parti.cfg", NULL, 20);
	ul_dump_text("/vendor/etc/init/qseecomd.rc", NULL, 40);

	/*
	 * keymaster-4-0 now gets through qseecom (TA load ioctls return 0) and
	 * then asks for /vendor/firmware_mnt/image/keymaster.mdt, which we
	 * answered -ENOENT.  Find out what that partition really contains.
	 */
	/* which dma-heaps did the board DT actually create? */
	ul_list_dir("/dev/dma_heap", NULL, 24);
	ul_exists("/dev/ion");
	ul_exists("/vendor/firmware_mnt/image/keymaster.mdt");
	/*
	 * positives control (adsp.mdt is in the listing) and every TA name the
	 * keymaster HAL might be looking for
	 */
	ul_exists("/vendor/firmware_mnt/image/adsp.mdt");
	ul_exists("/vendor/firmware_mnt/image/keymaster.b00");
	ul_exists("/vendor/firmware_mnt/image/keymaster64.mdt");
	ul_exists("/vendor/firmware_mnt/image/keymaster64.b00");
	ul_exists("/vendor/firmware_mnt/image/gatekeeper.mdt");
	ul_exists("/vendor/firmware_mnt/image/km.mdt");
	ul_exists("/vendor/firmware_mnt/image/Keymaster.mdt");
	ul_exists("/vendor/firmware_mnt/image/KEYMASTER.MDT");
	/*
	 * TA images are <name>.mdt + <name>.bNN, so the .mdt names are the
	 * complete TA list.  Keep it compact - the ring has to survive for the
	 * rest of the boot.
	 */
	ul_list_dir("/vendor/firmware_mnt/image", ".mdt", 60);
	/* what path/app name does the keymaster HAL ask qseecom for? */
	ul_dump_strings("/vendor/bin/hw/android.hardware.keymaster@4.0-service-qti",
			".mdt", 20);
	ul_dump_strings("/vendor/bin/hw/android.hardware.keymaster@4.0-service-qti",
			"firmware", 20);
	ul_dump_strings("/vendor/lib64/libQSEEComAPI.so", "firmware", 20);
	ul_dump_strings("/vendor/lib64/libQSEEComAPI.so", ".mdt", 20);

	pr_info("ULDUMP: done\n");
}

static DECLARE_DELAYED_WORK(ul_dump, ul_dump_work);

static int __init ul_dump_init(void)
{
	schedule_delayed_work(&ul_dump, msecs_to_jiffies(6000));
	return 0;
}
late_initcall(ul_dump_init);
