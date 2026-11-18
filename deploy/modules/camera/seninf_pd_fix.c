// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/of.h>

static int __init seninf_pd_fix_init(void)
{
	struct device_node *np;
	struct property *prop;

	np = of_find_node_by_path("/soc/seninf_top@1a004000");
	if (!np) {
		pr_warn("seninf_pd_fix: seninf_top not found\n");
		return 0;
	}

	prop = of_find_property(np, "power-domains", NULL);
	if (prop) {
		int ret = of_remove_property(np, prop);
		pr_info("seninf_pd_fix: removed power-domains from seninf_top, ret=%d\n", ret);
	} else {
		pr_info("seninf_pd_fix: power-domains already absent\n");
	}

	of_node_put(np);
	return 0;
}

static void __exit seninf_pd_fix_exit(void)
{
}

module_init(seninf_pd_fix_init);
module_exit(seninf_pd_fix_exit);
MODULE_LICENSE("GPL");
