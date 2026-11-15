# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_MTK_CMDQ) += mtk-cmdq-helper.o
obj-$(CONFIG_MTK_DEVAPC) += mtk-devapc.o
obj-$(CONFIG_MTK_INFRACFG) += mtk-infracfg.o
obj-y += mtk-pmic-wrap.o
obj-$(CONFIG_MTK_REGULATOR_COUPLER) += mtk-regulator-coupler.o
obj-$(CONFIG_MTK_MMSYS) += mtk-mmsys.o
obj-$(CONFIG_MTK_MMSYS) += mtk-mutex.o
obj-$(CONFIG_MTK_SVS) += mtk-svs.o
obj-$(CONFIG_MTK_SOCINFO) += mtk-socinfo.o
obj-y += mtk-dvfsrc-stub.o
obj-y += pmos-aee-stub.o
# MT6789 Mali-G57 DT fixup for Panfrost.  Re-enabled for the GPU bring-up; it
# must run at core_initcall, before of_platform_default_populate().
obj-y += mtk-mt6789-gpu-fixup.o
