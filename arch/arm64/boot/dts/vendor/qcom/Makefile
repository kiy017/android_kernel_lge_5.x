# SPDX-License-Identifier: GPL-2.0-only
# LG V60 ThinQ (timelm).  CONFIG_ARCH_QCOM forces -@ (scripts/Makefile.lib).
# One full DTB per LG 4.19 overlay, kona-timelm_<variant>_rev-<rev>.dtb (boot
# image, one rev per image), and the same revs as dtbo entries,
# kona-timelm_<variant>_rev-<rev>-overlay.dtbo, applied by LK to the base
# kona-timelm.dtb (boot image + dtbo image, all revs; see build-dtbo.sh).

# ROM builds (device/qcom/kernelscripts) pass CONFIG_BUILD_ARM64_DT_OVERLAY=y
# with BOARD_KERNEL_SEPARATED_DTBO and pack every *.dtb into dtb.img, so only
# the base is built there; the revs come from the dtbo image.
ifneq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
dtb-y += kona-timelm_dcm_jp_rev-0.0.dtb
dtb-y += kona-timelm_dcm_jp_rev-1.0.dtb
dtb-y += kona-timelm_dcm_jp_rev-a.dtb
dtb-y += kona-timelm_dcm_jp_rev-b.dtb
dtb-y += kona-timelm_dcm_jp_rev-b1.dtb
dtb-y += kona-timelm_dcm_jp_rev-b2.dtb
dtb-y += kona-timelm_dcm_jp_rev-c.dtb

dtb-y += kona-timelm_sb_jp_rev-0.0.dtb
dtb-y += kona-timelm_sb_jp_rev-1.0.dtb
dtb-y += kona-timelm_sb_jp_rev-a.dtb
dtb-y += kona-timelm_sb_jp_rev-b.dtb
dtb-y += kona-timelm_sb_jp_rev-b1.dtb
dtb-y += kona-timelm_sb_jp_rev-b2.dtb
dtb-y += kona-timelm_sb_jp_rev-c.dtb

dtb-y += kona-timelm_lao_com_rev-0.0.dtb
dtb-y += kona-timelm_lao_com_rev-1.0.dtb
dtb-y += kona-timelm_lao_com_rev-1.01.dtb
dtb-y += kona-timelm_lao_com_rev-1.02.dtb
dtb-y += kona-timelm_lao_com_rev-a.dtb
dtb-y += kona-timelm_lao_com_rev-b.dtb
dtb-y += kona-timelm_lao_com_rev-b1.dtb
dtb-y += kona-timelm_lao_com_rev-b2.dtb
dtb-y += kona-timelm_lao_com_rev-c.dtb
dtb-y += kona-timelm_lao_com_rev-d.dtb
dtb-y += kona-timelm_lao_com_rev-e.dtb

dtb-y += kona-timelm_lao_ldu_rev-0.0.dtb
dtb-y += kona-timelm_lao_ldu_rev-1.0.dtb
dtb-y += kona-timelm_lao_ldu_rev-a.dtb
dtb-y += kona-timelm_lao_ldu_rev-b.dtb
dtb-y += kona-timelm_lao_ldu_rev-b1.dtb
dtb-y += kona-timelm_lao_ldu_rev-b2.dtb
dtb-y += kona-timelm_lao_ldu_rev-c.dtb
dtb-y += kona-timelm_lao_ldu_rev-d.dtb
dtb-y += kona-timelm_lao_ldu_rev-e.dtb
endif

dtb-y += kona-timelm.dtb

dtbo-y += kona-timelm_dcm_jp_rev-0.0-overlay.dtbo
dtbo-y += kona-timelm_dcm_jp_rev-1.0-overlay.dtbo
dtbo-y += kona-timelm_dcm_jp_rev-a-overlay.dtbo
dtbo-y += kona-timelm_dcm_jp_rev-b-overlay.dtbo
dtbo-y += kona-timelm_dcm_jp_rev-b1-overlay.dtbo
dtbo-y += kona-timelm_dcm_jp_rev-b2-overlay.dtbo
dtbo-y += kona-timelm_dcm_jp_rev-c-overlay.dtbo
dtbo-y += kona-timelm_sb_jp_rev-0.0-overlay.dtbo
dtbo-y += kona-timelm_sb_jp_rev-1.0-overlay.dtbo
dtbo-y += kona-timelm_sb_jp_rev-a-overlay.dtbo
dtbo-y += kona-timelm_sb_jp_rev-b-overlay.dtbo
dtbo-y += kona-timelm_sb_jp_rev-b1-overlay.dtbo
dtbo-y += kona-timelm_sb_jp_rev-b2-overlay.dtbo
dtbo-y += kona-timelm_sb_jp_rev-c-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-0.0-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-1.0-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-1.01-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-1.02-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-a-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-b-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-b1-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-b2-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-c-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-d-overlay.dtbo
dtbo-y += kona-timelm_lao_com_rev-e-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-0.0-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-1.0-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-a-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-b-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-b1-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-b2-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-c-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-d-overlay.dtbo
dtbo-y += kona-timelm_lao_ldu_rev-e-overlay.dtbo

always-y	:= $(dtb-y) $(dtbo-y)
clean-files	:= *.dtb *.dtbo
