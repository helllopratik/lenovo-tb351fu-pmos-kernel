// SPDX-License-Identifier: GPL-2.0
/*
 * PMOS-TB351FU stub: claim LK-injected "mediatek,mt6789-dvfsrc" node and
 * register its always-on regulator children (dvfsrc-vcore / dvfsrc-vscp).
 *
 * Without this, fw_devlink sees "dvfsrc-vcore-supply" phandles on ~18
 * devices (msdc0/ufshci/mmdvfs/mdp/isp...) and defers them forever because
 * nothing ever binds the supplier node -> no SD, no UFS.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/of.h>
#include <linux/io.h>

static const struct regulator_ops dvfsrc_stub_reg_ops = {
	.list_voltage	= regulator_list_voltage_linear,
};

static int dvfsrc_stub_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	int nreg = 0;

	for_each_available_child_of_node(np, child) {
		struct regulator_desc desc;
		struct regulator_config cfg = { };
		struct regulator_dev *rdev;

		if (!of_property_read_bool(child, "regulator-name"))
			continue;

		memset(&desc, 0, sizeof(desc));
		desc.name = of_get_property(child, "regulator-name", NULL);
		if (!desc.name)
			continue;
		desc.of_match = child->name;
		desc.regulators_node = "dvfsrc";
		desc.id = nreg;
		desc.type = REGULATOR_VOLTAGE;
		desc.owner = THIS_MODULE;
		desc.ops = &dvfsrc_stub_reg_ops;
		desc.n_voltages = 1;
		desc.min_uV = 550000;
		desc.uV_step = 10000;

		cfg.dev = &pdev->dev;
		cfg.of_node = child;
		cfg.init_data = of_get_regulator_init_data(&pdev->dev, child, &desc);
		if (!cfg.init_data) {
			dev_warn(&pdev->dev, "PMOSDVFS: no init-data for %s\n", desc.name);
			continue;
		}
		desc.fixed_uV = cfg.init_data->constraints.min_uV ?
				cfg.init_data->constraints.min_uV : desc.min_uV;

		rdev = devm_regulator_register(&pdev->dev, &desc, &cfg);
		if (IS_ERR(rdev)) {
			dev_err(&pdev->dev, "PMOSDVFS: %s register failed: %ld\n",
				desc.name, PTR_ERR(rdev));
			continue;
		}
		nreg++;
		pr_info("PMOSDVFS: registered always-on regulator '%s' @%u uV\n",
			desc.name, desc.fixed_uV);
	}

	pr_info("PMOSDVFS: mt6789 dvfsrc stub probed, %d regulator(s)\n", nreg);
	return nreg ? 0 : -ENODEV;
}

static const struct of_device_id dvfsrc_stub_of_match[] = {
	{ .compatible = "mediatek,mt6789-dvfsrc" },
	{ }
};
MODULE_DEVICE_TABLE(of, dvfsrc_stub_of_match);

static struct platform_driver dvfsrc_stub_driver = {
	.probe = dvfsrc_stub_probe,
	.driver = {
		.name = "mt6789-dvfsrc-stub",
		.of_match_table = dvfsrc_stub_of_match,
	},
};
module_platform_driver(dvfsrc_stub_driver);
MODULE_LICENSE("GPL");
