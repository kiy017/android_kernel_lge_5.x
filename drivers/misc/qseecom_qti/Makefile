# timelm bring-up: QTI QSEECOM driver, built in-tree because this device has
# no vendor_boot / vendor_dlkm and we cannot ship qseecom.ko as a module.
#
# Sources come from android_kernel_xiaomi_sm8550-modules
# (qcom/opensource/securemsm-kernel).  Changes:
#  - <linux/ion.h> / <linux/msm_ion.h> removed (gone in 5.15; the driver only
#    passes dma-buf fds around and never calls the ion API)
#  - CONFIG_QSEECOM is force-defined: the local misc/qseecom_kernel.h would
#    otherwise provide a static inline stub for
#    qseecom_process_listener_from_smcinvoke() which qseecom.c also defines

obj-y += qseecom_qti.o
qseecom_qti-y := qseecom.o
qseecom_qti-$(CONFIG_COMPAT) += qseecom_32bit_impl.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_QSEECOM=1
