// SPDX-License-Identifier: GPL-2.0-only
/*
 * timelm: /dev/smcinvoke stub.  smcinvoke is not ported: the vendor TEE daemons
 * get -ENOSYS, and ssgtzd retries about once a second.
 */
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>

static long smc_dbg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	pr_info("NODEDBG[smcinvoke]: ioctl nr=%u cmd=0x%x comm=%s pid=%d\n",
		_IOC_NR(cmd), cmd, current->comm, task_pid_nr(current));
	return -ENOSYS;
}

static const struct file_operations smc_dbg_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = smc_dbg_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = smc_dbg_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice smc_dbg_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "smcinvoke",
	.fops = &smc_dbg_fops,
	.mode = 0666,
};

static int __init smc_dbg_init(void)
{
	int ret;

	ret = misc_register(&smc_dbg_dev);
	pr_info("NODEDBG: stub /dev/smcinvoke registered ret=%d\n", ret);
	return ret;
}
device_initcall(smc_dbg_init);
