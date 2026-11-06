// SPDX-License-Identifier: GPL-2.0-only
/*
 * timelm: legacy /dev/ion front end for the LG vendor userspace.
 *
 * Linux 5.15 has no ION at all (removed upstream in 5.11), but the LG V60
 * vendor blobs still speak the 4.19 msm ion ABI: libion.so (NEEDED by
 * libQSEEComAPI.so) opens /dev/ion and every TEE client - qseecomd's listener
 * libraries, keymaster, gatekeeper - allocates its shared buffer through it.
 *
 * The ABI was read out of the ROM's own libion.so:
 *
 *	ion_alloc():  0  u64 len
 *		      8  u64 align
 *		      16 u32 heap_id_mask
 *		      20 u32 flags
 *		      24 u32 handle	(out)		ioctl 0xc0204900 (nr 0)
 *	ion_share():  0  u32 handle
 *		      4  u32 fd		(out)		ioctl 0xc0084904 (nr 4)
 *	free/probe:   0  u32 handle			ioctl 0xc0044901 (nr 1)
 *
 * The stock 4.19 kernel only has the fd-in-struct ALLOC (24 bytes) and
 * HEAP_QUERY, so libion runs in its modern mode there.  ion_is_legacy() sees
 * FREE succeed here, so libion and the vendor libs use the legacy calls.  Both
 * are accepted.  There is no ION_IOC_CUSTOM in either: cache maintenance is
 * DMA_BUF_IOCTL_SYNC on the dma-buf fd.
 *
 * The heap mask matters: the vendor libs ask for ION_QSECOM_HEAP (bit 27,
 * ION_QSECOM_HEAP_ID = 27 in include/uapi/linux/msm_ion.h), i.e. physically
 * contiguous memory that qseecom can hand to the TEE as one physical range.
 * qseecom_set_client_mem_param() takes the *first sg entry* length as the
 * buffer size and rejects the request when the buffer is smaller:
 *
 *	QSEECOM: qseecom_set_client_mem_param: Requested length (0xa080) is > allocated (4096)
 *
 * which is exactly what the 4 KiB-page system heap gives.  So the mask is
 * mapped onto the qcom dma-heaps that the board DT already describes
 * (kona-dma-heaps.dtsi): qcom,qseecom / qcom,qseecom-ta / qcom,user-contig are
 * CMA-backed and export their buffers through mem_buf_dma_buf_export(), which
 * is also what qseecom_create_bridge_for_secbuf() and
 * mem_buf_dma_buf_exclusive_owner() expect.
 */
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/xarray.h>

#define ION_IOC_MAGIC			'I'
#define TIMELM_ION_NR_ALLOC		0
#define TIMELM_ION_NR_FREE			1
#define TIMELM_ION_NR_MAP			2
#define TIMELM_ION_NR_SHARE		4	/* 0xc0084904 in the ROM's libion */
#define TIMELM_ION_NR_IMPORT		5
#define TIMELM_ION_NR_SYNC			7
#define TIMELM_ION_NR_HEAP_QUERY		8

/* include/uapi/linux/msm_ion.h of the 4.19 LG kernel */
#define ION_QSECOM_TA_HEAP_BIT		(1u << 19)
#define ION_USER_CONTIG_HEAP_BIT	(1u << 26)
#define ION_QSECOM_HEAP_BIT		(1u << 27)

/* ion_allocation_data.flags (include/uapi/linux/ion.h) */
#define ION_FLAG_CACHED			1u

/*
 * The 32 byte layout, read straight out of the ROM's libion.so
 * (ion_alloc() builds it at sp+8: len, align, heap_id_mask, flags, handle).
 */
struct ion_allocation_data {
	__u64 len;
	__u64 align;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 handle;
	__u32 pad;
};

/* LG's own 4.19 uapi: fd comes back in the struct, 24 bytes */
struct ion_allocation_data_fd {
	__u64 len;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 fd;
	__u32 unused;
};

/*
 * The same legacy libion built for a 32-bit process (the audio HAL and its
 * ACDB loader): size_t len/align are 4 bytes, 20 bytes total, handle at 16.
 * On 4.19 compat_ion.c translated it; here the shim has to.
 */
struct ion_allocation_data32 {
	__u32 len;
	__u32 align;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 handle;
};

/* ION_IOC_SHARE/MAP: { ion_user_handle_t handle; int fd; } */
struct ion_fd_data {
	__u32 handle;
	__u32 fd;
};

struct ion_heap_data {
	char name[32];
	__u32 type;
	__u32 heap_id;
	__u32 reserved0;
	__u32 reserved1;
	__u32 reserved2;
};

struct ion_heap_query {
	__u32 cnt;
	__u32 reserved0;
	__u64 heaps;
	__u32 reserved1;
	__u32 reserved2;
};

enum timelm_ion_heap_id {
	TIMELM_ION_HEAP_QSECOM,
	TIMELM_ION_HEAP_QSECOM_TA,
	TIMELM_ION_HEAP_USER_CONTIG,
	TIMELM_ION_HEAP_SYSTEM,
	TIMELM_ION_HEAP_SYSTEM_UNCACHED,
	TIMELM_ION_HEAP_MAX,
};

static const char *const timelm_ion_heap_names[TIMELM_ION_HEAP_MAX] = {
	"qcom,qseecom",
	"qcom,qseecom-ta",
	"qcom,user-contig",
	"system",
	"system-uncached",
};

static struct dma_heap *timelm_ion_heaps[TIMELM_ION_HEAP_MAX];

/*
 * Legacy ION handles are per-/dev/ion-open ids, not fds: ION_IOC_SHARE/MAP
 * install a *new* fd each time, ION_IOC_FREE drops the handle, and the caller
 * closes its share/map fds on its own.  Codec2's C2AllocatorIon (mediaswcodec)
 * closes both its share fd and its map fd, so handing the same fd back for
 * handle, share and map (img<=68) double-closed it and a recycled fd number
 * blew up in hidl_handle: system_server "Failed to clone native_handle in
 * hidl_handle: Bad file descriptor" -> zygote restart, audio HAL killed.
 */
struct timelm_ion_client {
	struct xarray handles;		/* handle id -> struct dma_buf * (own ref) */
};

static int timelm_ion_new_handle(struct timelm_ion_client *c, struct dma_buf *dmabuf,
			      u32 *handle)
{
	return xa_alloc(&c->handles, handle, dmabuf, xa_limit_31b, GFP_KERNEL);
}

/* new fd for a handle; the fd owns its own dma_buf ref */
static int timelm_ion_handle_to_fd(struct timelm_ion_client *c, u32 handle)
{
	struct dma_buf *dmabuf;
	int fd;

	xa_lock(&c->handles);
	dmabuf = xa_load(&c->handles, handle);
	if (dmabuf)
		get_dma_buf(dmabuf);
	xa_unlock(&c->handles);
	if (!dmabuf)
		return -EINVAL;

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);
	return fd;
}
static int timelm_ion_logged;

/*
 * drivers/misc is linked before drivers/dma-buf, so no heap can be looked up at
 * initcall time (system_heap_create()/the qcom dma-heap probe are later).  All
 * lookups are lazy, on first use, and the result is cached.
 */
static struct dma_heap *timelm_ion_get_heap(enum timelm_ion_heap_id id)
{
	if (timelm_ion_heaps[id])
		return timelm_ion_heaps[id];

	timelm_ion_heaps[id] = dma_heap_find(timelm_ion_heap_names[id]);
	if (timelm_ion_heaps[id])
		pr_info("IONDBG: dma-heap %s found\n", timelm_ion_heap_names[id]);
	return timelm_ion_heaps[id];
}

/*
 * The vendor gralloc and the Adreno UMD allocate every graphics buffer through
 * here (ION_SYSTEM_HEAP, mask 0x2000000).  Without ION_FLAG_CACHED they treat
 * the buffer as uncached and never do cache maintenance, so it must come from
 * the write-combined system-uncached heap: from the cached "system" heap, CPU
 * writes (glyph atlases, decoded bitmaps) stay in the CPU cache and the GPU
 * samples stale DRAM (img48: missing glyphs, black tiles in the wallpaper).
 */
static enum timelm_ion_heap_id timelm_ion_heap_for_mask(u32 mask, u32 flags)
{
	if (mask & ION_QSECOM_HEAP_BIT)
		return TIMELM_ION_HEAP_QSECOM;
	if (mask & ION_QSECOM_TA_HEAP_BIT)
		return TIMELM_ION_HEAP_QSECOM_TA;
	if (mask & ION_USER_CONTIG_HEAP_BIT)
		return TIMELM_ION_HEAP_USER_CONTIG;
	if (!(flags & ION_FLAG_CACHED))
		return TIMELM_ION_HEAP_SYSTEM_UNCACHED;
	return TIMELM_ION_HEAP_SYSTEM;
}

static int timelm_ion_alloc(struct timelm_ion_client *c, unsigned long arg,
			 unsigned int size)
{
	struct ion_allocation_data data = { 0 };
	struct ion_allocation_data_fd data_fd = { 0 };
	struct ion_allocation_data32 data32 = { 0 };
	struct dma_heap *heap;
	enum timelm_ion_heap_id id;
	u64 len;
	u32 mask, flags, handle;
	struct dma_buf *dmabuf;
	int fd, ret;

	if (size >= sizeof(data)) {
		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		len = data.len;
		mask = data.heap_id_mask;
		flags = data.flags;
	} else if (size >= sizeof(data_fd)) {
		if (copy_from_user(&data_fd, (void __user *)arg,
				   sizeof(data_fd)))
			return -EFAULT;
		len = data_fd.len;
		mask = data_fd.heap_id_mask;
		flags = data_fd.flags;
	} else if (size == sizeof(data32)) {
		if (copy_from_user(&data32, (void __user *)arg,
				   sizeof(data32)))
			return -EFAULT;
		len = data32.len;
		mask = data32.heap_id_mask;
		flags = data32.flags;
	} else {
		pr_info("IONDBG: alloc with odd size %u\n", size);
		return -EINVAL;
	}

	id = timelm_ion_heap_for_mask(mask, flags);
	heap = timelm_ion_get_heap(id);
	if (!heap && id != TIMELM_ION_HEAP_SYSTEM) {
		pr_info("IONDBG: heap %s missing, falling back to system\n",
			timelm_ion_heap_names[id]);
		id = TIMELM_ION_HEAP_SYSTEM;
		heap = timelm_ion_get_heap(id);
	}
	if (!heap) {
		pr_info("IONDBG: no dma-heap at all for mask 0x%x\n", mask);
		return -ENODEV;
	}

	pr_info("IONDBG: ALLOC len=%llu mask=0x%x flags=0x%x size=%u heap=%s\n",
		(unsigned long long)len, mask, flags, size,
		timelm_ion_heap_names[id]);

	/* LG's own 24 byte layout is the fd-in-struct ABI: no handle at all */
	if (size < sizeof(data) && size != sizeof(data32)) {
		fd = dma_heap_bufferfd_alloc(heap, len, O_RDWR | O_CLOEXEC, 0);
		if (fd < 0) {
			pr_info("IONDBG: ALLOC len=%llu heap=%s failed ret=%d\n",
				(unsigned long long)len, timelm_ion_heap_names[id],
				fd);
			return fd;
		}
		if (put_user((u32)fd, (u32 __user *)(arg + 16))) {
			close_fd(fd);
			return -EFAULT;
		}
		pr_info("IONDBG: ALLOC len=%llu -> fd %d (heap %s)\n",
			(unsigned long long)len, fd, timelm_ion_heap_names[id]);
		return 0;
	}

	/* legacy layouts (32 B 64-bit, 20 B 32-bit): return a handle */
	dmabuf = dma_heap_buffer_alloc(heap, len, O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(dmabuf)) {
		pr_info("IONDBG: ALLOC len=%llu heap=%s failed ret=%ld\n",
			(unsigned long long)len, timelm_ion_heap_names[id],
			PTR_ERR(dmabuf));
		return PTR_ERR(dmabuf);
	}
	ret = timelm_ion_new_handle(c, dmabuf, &handle);
	if (ret) {
		dma_buf_put(dmabuf);
		return ret;
	}
	/* 32 byte layout: handle at 24; 32-bit 20 byte layout: handle at 16 */
	if (put_user(handle, (u32 __user *)(arg + (size >= sizeof(data) ?
						   24 : 16)))) {
		dma_buf_put(xa_erase(&c->handles, handle));
		return -EFAULT;
	}

	pr_info("IONDBG: ALLOC len=%llu -> handle %u (heap %s)\n",
		(unsigned long long)len, handle, timelm_ion_heap_names[id]);
	return 0;
}

/* ION_IOC_SHARE / ION_IOC_MAP: handle -> a new dma-buf fd */
static int timelm_ion_share(struct timelm_ion_client *c, unsigned long arg,
			 unsigned int size)
{
	struct ion_fd_data data;
	int fd;

	if (size < sizeof(data))
		return -EINVAL;
	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;

	fd = timelm_ion_handle_to_fd(c, data.handle);
	if (fd < 0) {
		pr_info("IONDBG: SHARE bad handle %u (%d)\n", data.handle, fd);
		return fd;
	}
	data.fd = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		close_fd(fd);
		return -EFAULT;
	}
	return 0;
}

/* ION_IOC_IMPORT: dma-buf fd -> new handle (holding its own ref) */
static int timelm_ion_import(struct timelm_ion_client *c, unsigned long arg,
			  unsigned int size)
{
	struct ion_fd_data data;
	struct dma_buf *dmabuf;
	u32 handle;
	int ret;

	if (size < sizeof(data))
		return -EINVAL;
	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;

	dmabuf = dma_buf_get(data.fd);
	if (IS_ERR(dmabuf)) {
		pr_info("IONDBG: IMPORT bad fd %u\n", data.fd);
		return -EINVAL;
	}
	ret = timelm_ion_new_handle(c, dmabuf, &handle);
	if (ret) {
		dma_buf_put(dmabuf);
		return ret;
	}
	data.handle = handle;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		dma_buf_put(xa_erase(&c->handles, handle));
		return -EFAULT;
	}
	return 0;
}

/* ION_IOC_FREE: { ion_user_handle_t handle; } */
static int timelm_ion_free(struct timelm_ion_client *c, unsigned long arg)
{
	struct dma_buf *dmabuf;
	u32 handle;

	if (get_user(handle, (u32 __user *)arg))
		return -EFAULT;
	/* handle 0 = libion's ion_is_legacy() probe: anything but -ENOTTY */
	if (!handle)
		return 0;
	dmabuf = xa_erase(&c->handles, handle);
	if (!dmabuf)
		return -EINVAL;
	dma_buf_put(dmabuf);
	return 0;
}

/* ION_IOC_SYNC: legacy "flush CPU writes for the device" on a dma-buf fd */
static int timelm_ion_sync(unsigned long arg, unsigned int size)
{
	struct ion_fd_data data;
	struct dma_buf *dmabuf;
	int ret;

	if (size < sizeof(data))
		return -EINVAL;
	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;

	dmabuf = dma_buf_get(data.fd);
	if (IS_ERR(dmabuf))
		return -EINVAL;
	ret = dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_put(dmabuf);
	return ret;
}

static int timelm_ion_heap_query(unsigned long arg)
{
	struct ion_heap_query q;
	struct ion_heap_data hd = { 0 };
	struct dma_heap *heap = timelm_ion_get_heap(TIMELM_ION_HEAP_SYSTEM);
	int ret = 0;

	if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
		return -EFAULT;

	if (heap)
		strscpy(hd.name, dma_heap_get_name(heap), sizeof(hd.name));
	if (q.cnt && q.heaps && heap &&
	    copy_to_user((void __user *)q.heaps, &hd, sizeof(hd)))
		return -EFAULT;
	q.cnt = heap ? 1 : 0;
	if (copy_to_user((void __user *)arg, &q, sizeof(q)))
		return -EFAULT;
	return ret;
}

static long timelm_ion_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct timelm_ion_client *c = file->private_data;
	unsigned int nr = _IOC_NR(cmd), size = _IOC_SIZE(cmd);
	int ret;

	if (timelm_ion_logged++ < 40)
		pr_info("IONDBG: ioctl nr=%u size=%u cmd=0x%x comm=%s pid=%d\n",
			nr, size, cmd, current->comm, task_pid_nr(current));

	if (_IOC_TYPE(cmd) != ION_IOC_MAGIC) {
		pr_info("IONDBG: unhandled ioctl cmd=0x%x -> -ENOTTY\n", cmd);
		return -ENOTTY;
	}

	switch (nr) {
	case TIMELM_ION_NR_ALLOC:
		ret = timelm_ion_alloc(c, arg, size);
		break;
	case TIMELM_ION_NR_FREE:
		ret = timelm_ion_free(c, arg);
		break;
	case TIMELM_ION_NR_MAP:
	case TIMELM_ION_NR_SHARE:
		ret = timelm_ion_share(c, arg, size);
		break;
	case TIMELM_ION_NR_IMPORT:
		ret = timelm_ion_import(c, arg, size);
		break;
	case TIMELM_ION_NR_SYNC:
		ret = timelm_ion_sync(arg, size);
		break;
	case TIMELM_ION_NR_HEAP_QUERY:
		ret = timelm_ion_heap_query(arg);
		break;
	default:
		/* 4.19 has no ION_IOC_CUSTOM; cache ops are DMA_BUF_IOCTL_SYNC */
		pr_info("IONDBG: unhandled ioctl nr=%u size=%u -> -ENOTTY\n",
			nr, size);
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static int timelm_ion_open(struct inode *inode, struct file *file)
{
	struct timelm_ion_client *c = kzalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return -ENOMEM;
	xa_init_flags(&c->handles, XA_FLAGS_ALLOC1);
	file->private_data = c;
	pr_info("IONDBG: /dev/ion open comm=%s pid=%d\n", current->comm,
		task_pid_nr(current));
	return 0;
}

static int timelm_ion_release(struct inode *inode, struct file *file)
{
	struct timelm_ion_client *c = file->private_data;
	struct dma_buf *dmabuf;
	unsigned long idx;

	xa_for_each(&c->handles, idx, dmabuf)
		dma_buf_put(dmabuf);
	xa_destroy(&c->handles);
	kfree(c);
	return 0;
}

static const struct file_operations timelm_ion_fops = {
	.owner = THIS_MODULE,
	.open = timelm_ion_open,
	.release = timelm_ion_release,
	.unlocked_ioctl = timelm_ion_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = timelm_ion_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice timelm_ion_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ion",
	.fops = &timelm_ion_fops,
	.mode = 0666,
};

static int __init timelm_ion_init(void)
{
	int ret;

	ret = misc_register(&timelm_ion_dev);
	pr_info("IONDBG: /dev/ion registered ret=%d\n", ret);
	return ret;
}
device_initcall(timelm_ion_init);
