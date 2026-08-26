// SPDX-License-Identifier: GPL-2.0
/*
 * PMOS-TB351FU (MediaTek MT6789, Mali-G57 MC2) GPU device-tree fixup.
 *
 * The mali node the kernel boots with comes from the AVB-locked stock
 * vendor_boot DTBO, which is proprietary-shaped:
 *
 *	mali@13000000 {
 *		compatible = "mediatek,mali", "arm,mali-valhall";
 *		interrupt-names = "GPU", "MMU", "JOB", "EVENT", "PWR";
 *		operating-points-v2 = <...>;
 *		ged-supply = <&ged>;
 *		...no clocks...
 *	};
 *
 * Panfrost (mainline) requires a mainline-shaped node: a mediatek-specific
 * compatible, lowercase interrupt names (job/mmu/gpu/event), a "clocks"/
 * "clock-names" pair and a resolvable "mali" regulator, and rejects the
 * proprietary "ged-supply". We rewrite the node in-place on the unflattened
 * tree here, at core_initcall, which runs before of_platform_default_populate
 * (arch_initcall_sync) binds the panfrost platform device.
 *
 * The mfgcfg clock provider is node /soc/syscon@13fbf000
 * (compatible mediatek,mt6789-mfg, #clock-cells=<1>, driven by
 * drivers/clk/mediatek/clk-mt6789-mfgcfg.c, clock id 0 = subsys_bg3d).
 * The GPU regulator is /soc/pwrap@10026000/mt6366/mt6358regulator/buck-vgpu.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#define MALI_NODE_PATH		"/soc/mali@13000000"
#define MFGCFG_NODE_PATH	"/soc/syscon@13fbf000"
#define VGPU_REG_NODE_PATH	"/soc/pwrap@10026000/mt6366/mt6358regulator/buck-vgpu"

/* Compatible[2]: "mediatek,mt6789-mali" '\0' "arm,mali-valhall-jm" '\0' */
#define MT6789_MALI_COMPATIBLE_STR	"mediatek,mt6789-mali\0arm,mali-valhall-jm"
#define MT6789_MALI_COMPATIBLE_LEN	(sizeof(MT6789_MALI_COMPATIBLE_STR))

/* interrupt-names[4]: "gpu" "mmu" "job" "event" */
#define INTNAMES_STR	"gpu\0mmu\0job\0event"
#define INTNAMES_LEN	(sizeof(INTNAMES_STR))

/* clock-names[1]: "core" */
#define CLKNAMES_STR	"core"
#define CLKNAMES_LEN	(sizeof(CLKNAMES_STR))

static struct property *fixup_make_prop(const char *name, const void *value,
					size_t len)
{
	struct property *prop;

	prop = kzalloc(sizeof(*prop) + len, GFP_KERNEL);
	if (!prop)
		return NULL;

	prop->name = (char *)name;
	prop->length = len;
	prop->value = (char *)(prop + 1);
	memcpy(prop->value, value, len);

	return prop;
}

static int __init mtk_mt6789_gpu_fixup(void)
{
	struct device_node *mali, *mfgcfg, *vgpu;
	struct property *prop;
	__be32 cells[2];
	u32 ph_mfgcfg = 0, ph_vgpu = 0, ph_mali = 0;

	mali = of_find_node_by_path(MALI_NODE_PATH);
	if (!mali) {
		pr_info("pmos-gpu-fixup: %s not found, skipping\n",
			MALI_NODE_PATH);
		return 0;
	}

	mfgcfg = of_find_node_by_path(MFGCFG_NODE_PATH);
	vgpu = of_find_node_by_path(VGPU_REG_NODE_PATH);

	if (of_property_read_u32(mfgcfg, "phandle", &ph_mfgcfg) ||
	    of_property_read_u32(vgpu, "phandle", &ph_vgpu))
		pr_warn("pmos-gpu-fixup: missing mfgcfg/vgpu phandle\n");
	else {
		/* clocks = <&mfgcfg 0> */
		cells[0] = cpu_to_be32(ph_mfgcfg);
		cells[1] = cpu_to_be32(0);
		prop = fixup_make_prop("clocks", cells, sizeof(cells));
		if (prop) {
			if (of_update_property(mali, prop))
				pr_err("pmos-gpu-fixup: add clocks failed\n");
		}

		prop = fixup_make_prop("clock-names", CLKNAMES_STR, CLKNAMES_LEN);
		if (prop) {
			if (of_update_property(mali, prop))
				pr_err("pmos-gpu-fixup: add clock-names failed\n");
		}

		/* mali-supply = <&vgpu> */
		cells[0] = cpu_to_be32(ph_vgpu);
		prop = fixup_make_prop("mali-supply", cells, sizeof(__be32));
		if (prop) {
			if (of_update_property(mali, prop))
				pr_err("pmos-gpu-fixup: add mali-supply failed\n");
		}
	}

	/* compatible */
	prop = fixup_make_prop("compatible", MT6789_MALI_COMPATIBLE_STR,
			       MT6789_MALI_COMPATIBLE_LEN);
	if (prop) {
		if (of_update_property(mali, prop))
			pr_err("pmos-gpu-fixup: update compatible failed\n");
	}

	/* interrupt-names -> lowercase mainline set */
	prop = fixup_make_prop("interrupt-names", INTNAMES_STR, INTNAMES_LEN);
	if (prop) {
		if (of_update_property(mali, prop))
			pr_err("pmos-gpu-fixup: update interrupt-names failed\n");
	}

	/* remove proprietary ged-supply and the reference OPP table */
	prop = of_find_property(mali, "ged-supply", NULL);
	if (prop) {
		if (of_remove_property(mali, prop))
			pr_err("pmos-gpu-fixup: remove ged-supply failed\n");
	}
	prop = of_find_property(mali, "operating-points-v2", NULL);
	if (prop) {
		if (of_remove_property(mali, prop))
			pr_err("pmos-gpu-fixup: remove operating-points-v2 failed\n");
	}

	of_property_read_u32(mali, "phandle", &ph_mali);
	pr_info("pmos-gpu-fixup: mali phandle=%08x rewritten for panfrost (mfgcfg=%08x vgpu=%08x)\n",
		ph_mali, ph_mfgcfg, ph_vgpu);

	of_node_put(mali);
	of_node_put(mfgcfg);
	of_node_put(vgpu);

	return 0;
}
core_initcall(mtk_mt6789_gpu_fixup);

MODULE_LICENSE("GPL");
