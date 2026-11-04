# SPDX-License-Identifier: GPL-2.0-only
#
# Downstream Qualcomm techpacks built directly into the kernel image.
# timelm (LG V60) has no vendor_boot partition, so anything display related
# cannot be shipped as a first-stage DLKM; it has to be linked in.
obj-y += display/

# timelm: vendor modules from android_kernel_lge_sm8250-modules, built in the same pass as the
# kernel's own modules (one modpost run, so cross-module symbols such as
# btpower -> cnss_utils resolve without any build ordering).
obj-$(CONFIG_TIMELM_WLAN)		+= wlan/
obj-$(CONFIG_TIMELM_BT)			+= bt/
obj-$(CONFIG_TIMELM_CAMERA)		+= camera/
obj-$(CONFIG_TIMELM_VIDEO)		+= video/
obj-$(CONFIG_TIMELM_CVP)		+= cvp/
obj-$(CONFIG_TIMELM_DATARMNET)		+= datarmnet/
obj-$(CONFIG_TIMELM_AUDIO)		+= audio/
obj-$(CONFIG_TIMELM_FINGERPRINT)	+= fingerprint/
obj-$(CONFIG_TIMELM_NFC)		+= nfc/
obj-$(CONFIG_TIMELM_QCE)		+= qce/
obj-$(CONFIG_TIMELM_KGSL)		+= kgsl/
