/*
 * touch_common.c
 *
 * Copyright (c) 2015 LGE.
 *
 * author : hoyeon.jang@lge.com
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/input/lge_touch_notify.h>

/*
 *  Include to touch core Header File
 */
#include <touch_core.h>
#include <touch_hwif.h>
#include <touch_common.h>

void touch_msleep(unsigned int msecs)
{
	if (msecs >= 20)
		msleep(msecs);
	else
		usleep_range(msecs * 1000, msecs * 1000);
}

void touch_interrupt_control(struct device *dev, int on_off)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	if (on_off) {
		if (atomic_cmpxchg(&ts->state.irq_enable, 0, 1) == 0) {
			touch_enable_irq(ts->irq);

			if (ts->role.use_lpwg)
				touch_enable_irq_wake(ts->irq);
		}
	} else {
		if (atomic_cmpxchg(&ts->state.irq_enable, 1, 0) == 1) {
			if (ts->role.use_lpwg)
				touch_disable_irq_wake(ts->irq);

			touch_disable_irq(ts->irq);
		}
	}
}

int touch_snprintf(char *buf, int size, const char *fmt, ...)
{

	va_list args;
	int ret = 0;

	if(size <= 0) {
		TOUCH_E("size is negative!\n");
		return 0;
	}

	va_start(args, fmt);
	ret = vsnprintf(buf, (size_t)size, fmt, args);
	va_end(args);

	if (ret < 0) {
		TOUCH_E("snprintf error. change ret value(%d -> 0)\n", ret);
		ret = 0;
	}

	return ret;
}


/*
 * Self-test log helpers.  LG wrote these files with set_fs(KERNEL_DS) and the
 * ksys_*() syscall wrappers, none of which exist on 5.15; use filp_open() +
 * kernel_write() instead.  Rotation needed in-kernel link()/unlink(), so an
 * oversized log is truncated instead.
 */
int touch_file_append(const char *fname, const char *data, size_t len)
{
	struct file *file;
	loff_t pos;
	ssize_t ret;

	file = filp_open(fname, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (IS_ERR(file)) {
		TOUCH_I("%s : ERR(%ld) Open file error [%s]\n", __func__,
				PTR_ERR(file), fname);
		return PTR_ERR(file);
	}

	pos = i_size_read(file_inode(file));
	ret = kernel_write(file, data, len, &pos);
	filp_close(file, NULL);

	return ret < 0 ? ret : 0;
}

void touch_file_trim(const char *fname, loff_t max_size)
{
	struct file *file;
	loff_t file_size;

	file = filp_open(fname, O_RDONLY, 0);
	if (IS_ERR(file)) {
		TOUCH_I("%s : ERR(%ld) Open file error [%s]\n", __func__,
				PTR_ERR(file), fname);
		return;
	}
	file_size = i_size_read(file_inode(file));
	filp_close(file, NULL);

	TOUCH_I("%s : [%s] file_size = %lld\n", __func__, fname, file_size);
	if (file_size <= max_size)
		return;

	file = filp_open(fname, O_WRONLY | O_TRUNC, 0);
	if (!IS_ERR(file)) {
		TOUCH_I("%s : [%s] truncated (was %lld bytes)\n", __func__,
				fname, file_size);
		filp_close(file, NULL);
	}
}

void touch_file_time_string(char *buf, size_t len)
{
	struct timespec64 now;
	struct tm date;

	ktime_get_real_ts64(&now);
	time64_to_tm(now.tv_sec, sys_tz.tz_minuteswest * 60 * (-1), &date);
	snprintf(buf, len, "\n[%02d-%02d %02d:%02d:%02d.%03lu]\n",
			date.tm_mon + 1, date.tm_mday, date.tm_hour,
			date.tm_min, date.tm_sec,
			(unsigned long)now.tv_nsec / 1000000);
}
