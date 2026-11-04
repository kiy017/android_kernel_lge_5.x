/* production_test.c
 *
 * Copyright (C) 2015 LGE.
 *
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
#include <linux/delay.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>

/*
 *  Include to touch core Header File
 */
#include <touch_hwif.h>
#include <touch_core.h>

#define MODULE_MAX_LOG_FILE_SIZE	(10 * 1024 * 1024) /* 10 M byte */
#define MODULE_MAX_LOG_FILE_COUNT	5
#define MODULE_FILE_STR_LEN		(128)

enum {
	TIME_INFO_SKIP,
	TIME_INFO_WRITE,
};

void module_write_file(struct device *dev, char *data, int write_time)
{
	char *fname = NULL;
	char time_string[64] = {0};
	int boot_mode = TOUCH_NORMAL_BOOT;
	struct touch_core_data *ts;

	TOUCH_TRACE();

	ts = (struct touch_core_data *) plist->touch_core_data;

	boot_mode = touch_check_boot_mode(ts->dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
		fname = "/data/vendor/touch/touch_module_self_test.txt";
		break;
	case TOUCH_MINIOS_AAT:
		fname = "/data/touch/touch_module_self_test.txt";
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		fname = "/data/touch/touch_module_self_mfts.txt";
		break;
	default:
		TOUCH_I("%s : not support mode\n", __func__);
		break;
	}


	if (!fname) {
		TOUCH_E("%s : fname is NULL, can not open FILE\n", __func__);
		return;
	}

	if (write_time == TIME_INFO_WRITE) {
		touch_file_time_string(time_string, sizeof(time_string));
		touch_file_append(fname, time_string, strlen(time_string));
	}
	touch_file_append(fname, data, strlen(data));
}

void module_log_file_size_check(struct device *dev)
{
	char *fname = NULL;
	int boot_mode = TOUCH_NORMAL_BOOT;

	TOUCH_TRACE();


	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
		fname = "/data/vendor/touch/touch_module_self_test.txt";
		break;
	case TOUCH_MINIOS_AAT:
		fname = "/data/touch/touch_module_self_test.txt";
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		fname = "/data/touch/touch_module_self_mfts.txt";
		break;
	default:
		TOUCH_I("%s : not support mode\n", __func__);
		break;
	}

	if (!fname) {
		TOUCH_E("%s : fname is NULL, can not open FILE\n", __func__);
		return;
	}

	/* 5.15 has no in-kernel link()/unlink(): truncate instead of rotating. */
	touch_file_trim(fname, MODULE_MAX_LOG_FILE_SIZE);
}
