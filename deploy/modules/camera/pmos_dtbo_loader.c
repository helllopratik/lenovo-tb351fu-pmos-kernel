// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/firmware.h>

static int ov_id = -1;
static char *dtbo = "camera_ports.dtbo";
module_param(dtbo, charp, 0644);
MODULE_PARM_DESC(dtbo, "Name of DTBO firmware file in /lib/firmware");

static int __init dtbo_loader_init(void)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, dtbo, NULL);
	if (ret) {
		pr_err("pmos_dtbo_loader: failed to load firmware '%s': %d\n", dtbo, ret);
		return ret;
	}

	pr_info("pmos_dtbo_loader: applying %s (%zu bytes)\n", dtbo, fw->size);
	ret = of_overlay_fdt_apply((void *)fw->data, fw->size, &ov_id, NULL);
	if (ret) {
		pr_err("pmos_dtbo_loader: of_overlay_fdt_apply failed: %d\n", ret);
	} else {
		pr_info("pmos_dtbo_loader: overlay applied successfully, ov_id=%d\n", ov_id);
	}

	release_firmware(fw);
	return ret;
}

static void __exit dtbo_loader_exit(void)
{
	if (ov_id >= 0) {
		pr_info("pmos_dtbo_loader: removing overlay ov_id=%d\n", ov_id);
		of_overlay_remove(&ov_id);
		ov_id = -1;
	}
}

module_init(dtbo_loader_init);
module_exit(dtbo_loader_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apply DTBO overlay from /lib/firmware");
