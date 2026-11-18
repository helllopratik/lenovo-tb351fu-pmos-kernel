// SPDX-License-Identifier: GPL-2.0
/*
 * mt6789_pstore_arm - register the TB351FU pstore/ramoops block at runtime
 *
 * The stock Android bootargs point ramoops at 0x48090000 (size 0xe0000,
 * console 0x40000, pmsg 0x80000), but the pmOS boot image cmdline does not
 * carry those parameters and the DT node is "mediatek,pstore" rather than
 * "ramoops".  The builtin ramoops driver therefore never registers and
 * crashes leave nothing behind.
 *
 * This module registers a "ramoops" platform device with equivalent
 * platform data.  Load it before triggering a crash; after reboot load it
 * again (or parse the raw block) to read the preserved console zone.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kmsg_dump.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pstore_ram.h>

#define PSTORE_PHYS	0x48090000
#define PSTORE_SIZE	0xe0000
#define PSTORE_RECORD	0x20000
#define PSTORE_CONSOLE	0x40000
#define PSTORE_PMSG	0x80000

static struct ramoops_platform_data pstore_pdata = {
	.mem_size	= PSTORE_SIZE,
	.mem_address	= PSTORE_PHYS,
	.mem_type	= 0,
	.record_size	= PSTORE_RECORD,
	.console_size	= PSTORE_CONSOLE,
	.pmsg_size	= PSTORE_PMSG,
	.ftrace_size	= 0,
	.max_reason	= KMSG_DUMP_PANIC,
	.flags		= 0,
};

static struct resource pstore_res = {
	.start	= PSTORE_PHYS,
	.end	= PSTORE_PHYS + PSTORE_SIZE - 1,
	.flags	= IORESOURCE_MEM,
	.name	= "ramoops",
};

static struct platform_device *pstore_pdev;

static int __init mt6789_pstore_arm_init(void)
{
	int ret;

	pstore_pdev = platform_device_register_resndata(NULL, "ramoops", -1,
						       &pstore_res, 1,
						       &pstore_pdata,
						       sizeof(pstore_pdata));
	if (IS_ERR(pstore_pdev)) {
		ret = PTR_ERR(pstore_pdev);
		pr_err("mt6789_pstore_arm: register failed: %d\n", ret);
		pstore_pdev = NULL;
		return ret;
	}

	pr_info("mt6789_pstore_arm: ramoops armed at %#x size %#x (console %#x pmsg %#x)\n",
		PSTORE_PHYS, PSTORE_SIZE, PSTORE_CONSOLE, PSTORE_PMSG);
	return 0;
}

static void __exit mt6789_pstore_arm_exit(void)
{
	if (pstore_pdev)
		platform_device_unregister(pstore_pdev);
	pr_info("mt6789_pstore_arm: ramoops device removed\n");
}

module_init(mt6789_pstore_arm_init);
module_exit(mt6789_pstore_arm_exit);

MODULE_DESCRIPTION("Arm TB351FU ramoops/pstore block for pmOS crash capture");
MODULE_LICENSE("GPL");
