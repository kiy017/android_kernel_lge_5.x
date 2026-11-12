// SPDX-License-Identifier: GPL-2.0-only
/*
 * timelm: /dev/ion front end for the LG vendor userspace.
 *
 * Linux 5.15 has no ION at all (removed upstream in 5.11), but the LG V60
 * vendor blobs still allocate through libion: libion.so (NEEDED by
 * libQSEEComAPI.so) opens /dev/ion and every TEE client - qseecomd's listener
 * libraries, keymaster, gatekeeper - allocates its shared buffer through it,
 * as do the vendor gralloc, the Adreno UMD and the audio HAL.
 *
 * This is the ABI of the stock LG 4.19 kernel (drivers/staging/android/ion/
 * ion-ioctl.c), which matches commands exactly and accepts only
 *
 *	ION_IOC_ALLOC	   _IOWR('I', 0, 24 bytes)	fd returned in the struct
 *	ION_IOC_HEAP_QUERY _IOWR('I', 8, 24 bytes)
 *
 * (plus the 'M' secure-heap PREFETCH/DRAIN, which need secure heaps that do
 * not exist here).  libion's ion_is_legacy() probes ION_IOC_FREE: -ENOTTY
 * selects the fd-in-struct path above, as on stock.  The handle ABI (FREE,
 * SHARE, MAP, IMPORT, SYNC) does not exist on stock either.  There is no
 * ION_IOC_CUSTOM: cache maintenance is DMA_BUF_IOCTL_SYNC on the dma-buf fd.
 *
 * Buffers come from the qcom dma-heaps that the board DT describes
 * (kona-dma-heaps.dtsi).  The heap mask matters: the TEE clients ask for
 * ION_QSECOM_HEAP (bit 27), i.e. physically contiguous memory that qseecom can
 * hand to the TEE as one physical range.  qseecom_set_client_mem_param()
 * takes the *first sg entry* length as the buffer size and rejects the request
 * when the buffer is smaller:
 *
 *	QSEECOM: qseecom_set_client_mem_param: Requested length (0xa080) is > allocated (4096)
 *
 * which is exactly what the 4 KiB-page system heap gives.  qcom,qseecom,
 * qcom,qseecom-ta and qcom,user-contig are CMA-backed and export their buffers
 * through mem_buf_dma_buf_export(), which is also what
 * qseecom_create_bridge_for_secbuf() and mem_buf_dma_buf_exclusive_owner()
 * expect.
 */
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/dma-heap.h>

/* include/uapi/linux/ion.h of the 4.19 LG kernel */
struct ion_allocation_data {
	__u64 len;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 fd;
	__u32 unused;
};

#define ION_FLAG_CACHED			1u

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

#define ION_IOC_MAGIC			'I'
#define ION_IOC_ALLOC			_IOWR(ION_IOC_MAGIC, 0, \
					      struct ion_allocation_data)
#define ION_IOC_HEAP_QUERY		_IOWR(ION_IOC_MAGIC, 8, \
					      struct ion_heap_query)

#define ION_HEAP_TYPE_SYSTEM		0
#define ION_HEAP_TYPE_DMA		4

/* include/uapi/linux/msm_ion.h of the 4.19 LG kernel */
#define ION_QSECOM_TA_HEAP_ID		19
#define ION_SYSTEM_HEAP_ID		25
#define ION_USER_CONTIG_HEAP_ID		26
#define ION_QSECOM_HEAP_ID		27

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

/*
 * What HEAP_QUERY reports: the stock heaps (names from 4.19 ion.h, ids and
 * types from the stock DT) that are backed here, in stock DT order.  The
 * secure, adsp, spss and secure-display heaps have no counterpart.
 */
static const struct {
	const char *name;
	u32 type;
	u32 id;
} timelm_ion_query_heaps[] = {
	{ "system",	 ION_HEAP_TYPE_SYSTEM, ION_SYSTEM_HEAP_ID },
	{ "user_contig", ION_HEAP_TYPE_DMA,    ION_USER_CONTIG_HEAP_ID },
	{ "qsecom",	 ION_HEAP_TYPE_DMA,    ION_QSECOM_HEAP_ID },
	{ "qsecom_ta",	 ION_HEAP_TYPE_DMA,    ION_QSECOM_TA_HEAP_ID },
};

static struct dma_heap *timelm_ion_heaps[TIMELM_ION_HEAP_MAX];
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
	if (mask & BIT(ION_QSECOM_HEAP_ID))
		return TIMELM_ION_HEAP_QSECOM;
	if (mask & BIT(ION_QSECOM_TA_HEAP_ID))
		return TIMELM_ION_HEAP_QSECOM_TA;
	if (mask & BIT(ION_USER_CONTIG_HEAP_ID))
		return TIMELM_ION_HEAP_USER_CONTIG;
	if (!(flags & ION_FLAG_CACHED))
		return TIMELM_ION_HEAP_SYSTEM_UNCACHED;
	return TIMELM_ION_HEAP_SYSTEM;
}

static int timelm_ion_alloc(struct ion_allocation_data *data)
{
	enum timelm_ion_heap_id id;
	struct dma_heap *heap;
	int fd;

	id = timelm_ion_heap_for_mask(data->heap_id_mask, data->flags);
	heap = timelm_ion_get_heap(id);
	if (!heap && id != TIMELM_ION_HEAP_SYSTEM) {
		pr_info("IONDBG: heap %s missing, falling back to system\n",
			timelm_ion_heap_names[id]);
		id = TIMELM_ION_HEAP_SYSTEM;
		heap = timelm_ion_get_heap(id);
	}
	if (!heap) {
		pr_info("IONDBG: no dma-heap at all for mask 0x%x\n",
			data->heap_id_mask);
		return -ENODEV;
	}

	fd = dma_heap_bufferfd_alloc(heap, data->len, O_RDWR | O_CLOEXEC, 0);
	if (fd < 0) {
		pr_info("IONDBG: ALLOC len=%llu mask=0x%x flags=0x%x heap=%s failed ret=%d\n",
			(unsigned long long)data->len, data->heap_id_mask,
			data->flags, timelm_ion_heap_names[id], fd);
		return fd;
	}
	pr_info("IONDBG: ALLOC len=%llu mask=0x%x flags=0x%x -> fd %d (heap %s)\n",
		(unsigned long long)data->len, data->heap_id_mask, data->flags,
		fd, timelm_ion_heap_names[id]);

	data->fd = fd;
	return 0;
}

static int timelm_ion_heap_query(struct ion_heap_query *q)
{
	struct ion_heap_data __user *buf = u64_to_user_ptr(q->heaps);
	struct ion_heap_data hd;
	u32 i, cnt = 0;

	if (q->reserved0 || q->reserved1 || q->reserved2)
		return -EINVAL;

	if (!buf) {
		q->cnt = ARRAY_SIZE(timelm_ion_query_heaps);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(timelm_ion_query_heaps) && cnt < q->cnt; i++) {
		memset(&hd, 0, sizeof(hd));
		strscpy(hd.name, timelm_ion_query_heaps[i].name, sizeof(hd.name));
		hd.type = timelm_ion_query_heaps[i].type;
		hd.heap_id = timelm_ion_query_heaps[i].id;
		if (copy_to_user(&buf[cnt], &hd, sizeof(hd)))
			return -EFAULT;
		cnt++;
	}
	q->cnt = cnt;
	return 0;
}

static long timelm_ion_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	union {
		struct ion_allocation_data alloc;
		struct ion_heap_query query;
	} data;
	int ret;

	if (timelm_ion_logged++ < 40)
		pr_info("IONDBG: ioctl nr=%u size=%u cmd=0x%x comm=%s pid=%d\n",
			_IOC_NR(cmd), _IOC_SIZE(cmd), cmd, current->comm,
			task_pid_nr(current));

	switch (cmd) {
	case ION_IOC_ALLOC:
		if (copy_from_user(&data.alloc, (void __user *)arg,
				   sizeof(data.alloc)))
			return -EFAULT;
		ret = timelm_ion_alloc(&data.alloc);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &data.alloc,
				 sizeof(data.alloc))) {
			close_fd(data.alloc.fd);
			return -EFAULT;
		}
		return 0;
	case ION_IOC_HEAP_QUERY:
		if (copy_from_user(&data.query, (void __user *)arg,
				   sizeof(data.query)))
			return -EFAULT;
		ret = timelm_ion_heap_query(&data.query);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &data.query,
				 sizeof(data.query)))
			return -EFAULT;
		return 0;
	default:
		/* includes libion's ION_IOC_FREE probe: stock has no FREE */
		if (_IOC_TYPE(cmd) != ION_IOC_MAGIC || _IOC_NR(cmd) != 1)
			pr_info("IONDBG: unhandled ioctl cmd=0x%x comm=%s -> -ENOTTY\n",
				cmd, current->comm);
		return -ENOTTY;
	}
}

static const struct file_operations timelm_ion_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = timelm_ion_ioctl,
#ifdef CONFIG_COMPAT
	/* every field is fixed size, so 32-bit callers use the same layout */
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
