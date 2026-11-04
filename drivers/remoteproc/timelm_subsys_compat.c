// SPDX-License-Identifier: GPL-2.0-only
/*
 * timelm: 4.19 subsystem-restart userspace ABI on top of remoteproc.
 *
 * The LG V60 vendor partition is built for msm-4.19.  Its peripheral manager
 * (pm-service, libmdmdetect.so) finds the external modem through
 *   /sys/bus/msm_subsys/devices/subsysN/{name,state}
 * next to /sys/bus/esoc/devices/esocN/esoc_*, and powers it with
 *   open("/dev/subsys_<name>")	-> subsystem_get()
 *   close()			-> subsystem_put()
 * 5.15 has neither: the esoc is the remoteproc "esoc0" with auto_boot off.
 * This keeps that ABI for the rprocs registered here: open boots the rproc,
 * release shuts it down (both refcounted by remoteproc itself).  Functional
 * compat for the vendor userspace, not debug code.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timelm_subsys_compat.h>

#define TIMELM_SUBSYS_MAX	8

struct timelm_subsys {
	struct device dev;
	struct cdev cdev;
	struct device *char_dev;
	struct rproc *rproc;
	char name[32];
	char restart_level[16];
	int id;
};

static DEFINE_IDA(timelm_subsys_ida);
static dev_t timelm_subsys_devt;
static struct class *timelm_subsys_class;

#define to_timelm_subsys(d) container_of(d, struct timelm_subsys, dev)

static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", to_timelm_subsys(dev)->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct timelm_subsys *ss = to_timelm_subsys(dev);
	bool online = ss->rproc->state == RPROC_RUNNING ||
		      ss->rproc->state == RPROC_ATTACHED;

	return scnprintf(buf, PAGE_SIZE, "%s\n", online ? "ONLINE" : "OFFLINE");
}
static DEVICE_ATTR_RO(state);

static ssize_t crash_count_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}
static DEVICE_ATTR_RO(crash_count);

static ssize_t restart_level_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 to_timelm_subsys(dev)->restart_level);
}

static ssize_t restart_level_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct timelm_subsys *ss = to_timelm_subsys(dev);

	if (sysfs_streq(buf, "SYSTEM"))
		strscpy(ss->restart_level, "SYSTEM", sizeof(ss->restart_level));
	else if (sysfs_streq(buf, "RELATED"))
		strscpy(ss->restart_level, "RELATED", sizeof(ss->restart_level));
	else
		return -EPERM;
	return count;
}
static DEVICE_ATTR_RW(restart_level);

static ssize_t firmware_name_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct rproc *rproc = to_timelm_subsys(dev)->rproc;

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 rproc->firmware ? rproc->firmware : "");
}
static DEVICE_ATTR_RO(firmware_name);

static struct attribute *timelm_subsys_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_state.attr,
	&dev_attr_crash_count.attr,
	&dev_attr_restart_level.attr,
	&dev_attr_firmware_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(timelm_subsys);

static struct bus_type timelm_subsys_bus = {
	.name = "msm_subsys",
	.dev_groups = timelm_subsys_groups,
};

static int timelm_subsys_open(struct inode *inode, struct file *file)
{
	struct timelm_subsys *ss = container_of(inode->i_cdev,
						struct timelm_subsys, cdev);
	int ret;

	dev_info(&ss->dev, "%s: %s opens, booting %s\n", ss->name,
		 current->comm, dev_name(&ss->rproc->dev));
	ret = rproc_boot(ss->rproc);
	if (ret) {
		dev_err(&ss->dev, "%s: rproc_boot failed: %d\n", ss->name, ret);
		return ret;
	}
	file->private_data = ss;
	return 0;
}

static int timelm_subsys_release(struct inode *inode, struct file *file)
{
	struct timelm_subsys *ss = file->private_data;

	dev_info(&ss->dev, "%s: %s closes, shutting down\n", ss->name,
		 current->comm);
	rproc_shutdown(ss->rproc);
	return 0;
}

static const struct file_operations timelm_subsys_fops = {
	.owner = THIS_MODULE,
	.open = timelm_subsys_open,
	.release = timelm_subsys_release,
};

static void timelm_subsys_dev_release(struct device *dev)
{
	struct timelm_subsys *ss = to_timelm_subsys(dev);

	ida_free(&timelm_subsys_ida, ss->id);
	kfree(ss);
}

int timelm_subsys_compat_add(struct rproc *rproc, const char *name)
{
	struct timelm_subsys *ss;
	dev_t devt;
	int ret;

	if (!timelm_subsys_class)
		return -EPROBE_DEFER;

	ss = kzalloc(sizeof(*ss), GFP_KERNEL);
	if (!ss)
		return -ENOMEM;

	ss->id = ida_alloc_max(&timelm_subsys_ida, TIMELM_SUBSYS_MAX - 1,
			       GFP_KERNEL);
	if (ss->id < 0) {
		ret = ss->id;
		kfree(ss);
		return ret;
	}
	ss->rproc = rproc;
	strscpy(ss->name, name, sizeof(ss->name));
	strscpy(ss->restart_level, "RELATED", sizeof(ss->restart_level));

	ss->dev.bus = &timelm_subsys_bus;
	ss->dev.parent = &rproc->dev;
	ss->dev.release = timelm_subsys_dev_release;
	dev_set_name(&ss->dev, "subsys%d", ss->id);
	ret = device_register(&ss->dev);
	if (ret) {
		put_device(&ss->dev);
		return ret;
	}

	devt = MKDEV(MAJOR(timelm_subsys_devt), ss->id);
	cdev_init(&ss->cdev, &timelm_subsys_fops);
	ss->cdev.owner = THIS_MODULE;
	ret = cdev_add(&ss->cdev, devt, 1);
	if (ret)
		goto err_unregister;

	ss->char_dev = device_create(timelm_subsys_class, &ss->dev, devt, ss,
				     "subsys_%s", ss->name);
	if (IS_ERR(ss->char_dev)) {
		ret = PTR_ERR(ss->char_dev);
		goto err_cdev;
	}

	dev_info(&ss->dev, "%s -> %s, /dev/subsys_%s\n", ss->name,
		 dev_name(&rproc->dev), ss->name);
	return 0;

err_cdev:
	cdev_del(&ss->cdev);
err_unregister:
	device_unregister(&ss->dev);
	return ret;
}
EXPORT_SYMBOL(timelm_subsys_compat_add);

static int __init timelm_subsys_compat_init(void)
{
	int ret;

	ret = bus_register(&timelm_subsys_bus);
	if (ret)
		return ret;

	ret = alloc_chrdev_region(&timelm_subsys_devt, 0, TIMELM_SUBSYS_MAX,
				  "subsys");
	if (ret)
		goto err_bus;

	timelm_subsys_class = class_create(THIS_MODULE, "subsys");
	if (IS_ERR(timelm_subsys_class)) {
		ret = PTR_ERR(timelm_subsys_class);
		timelm_subsys_class = NULL;
		goto err_region;
	}
	return 0;

err_region:
	unregister_chrdev_region(timelm_subsys_devt, TIMELM_SUBSYS_MAX);
err_bus:
	bus_unregister(&timelm_subsys_bus);
	return ret;
}
postcore_initcall(timelm_subsys_compat_init);

MODULE_DESCRIPTION("timelm: 4.19 msm_subsys / subsys_<name> compat on remoteproc");
MODULE_LICENSE("GPL");
