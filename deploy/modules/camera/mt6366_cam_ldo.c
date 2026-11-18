// SPDX-License-Identifier: GPL-2.0
/*
 * mt6366_cam_ldo - enable MT6366 camera DOVDD/DVDD/AVDD/AFVDD rails directly
 *
 * The TB351FU vendor DTB wires cam0_vcamio/cam1_vcamio to ldo-vrf18
 * (1.8 V DOVDD) and the AVDD/DVDD load switches to VAUD28 (2.8 V) and
 * VA12 (1.2 V).  The vendor DTB also points cam0_vcamaf at ldo-vibr, but
 * this board has no vibrator hardware, so VIBR is never touched.  Mainline
 * mt6358-regulator declares those rails with the supply names
 * "vs1-ldo1"/"vsys-ldo3"/"vs2-ldo2"/"vsys-ldo1", but this board's DTB does
 * not define those parent rails.  The regulator core therefore attaches a
 * dummy regulator as the parent, and regulator_enable() fails with -EPERM
 * when it tries to enable that dummy before writing the LDO enable
 * bit.  The LDOs themselves are fully described (enable register 0x1c1c
 * bit 0 for VRF18, 0x1a9c bit 0 for VA12 and 0x1ac4 bit 0 for VAUD28),
 * so this helper programs the PMIC regmap directly
 * while the camera bring-up is using vendor DT.  VA12/VAUD28 are normally
 * handled by regulator-always-on, but the dummy-parent failure above stops
 * the core before it reaches their enable bits too.
 *
 * Loading this module is intentionally a separate, explicit step.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/mfd/mt6358/registers.h>

static bool enable_vrf18 = true;
module_param(enable_vrf18, bool, 0644);
MODULE_PARM_DESC(enable_vrf18, "set ldo-vrf18 enable bit (default 1)");

static bool disable_vrf18;
module_param(disable_vrf18, bool, 0644);
MODULE_PARM_DESC(disable_vrf18, "force ldo-vrf18 off for rail testing (default 0)");

/*
 * VA12 (1.2 V DVDD) and VAUD28 (2.8 V AVDD) are marked regulator-always-on
 * in the TB351FU DTB, but their parent supplies (vs2-ldo2 / vsys-ldo1) are
 * absent from this DTB.  regulator_register() therefore resolves a dummy
 * parent, and the always-on path fails at regulator_enable(rdev->supply)
 * before it ever reaches the LDO enable bit.  Program CON0 directly, exactly
 * as done for VRF18.  Both rails are shared with other consumers, so
 * the helper never disables them on unload.
 */
static bool enable_va12 = true;
module_param(enable_va12, bool, 0644);
MODULE_PARM_DESC(enable_va12, "set ldo-va12 enable bit (default 1)");

static bool enable_vaud28 = true;
module_param(enable_vaud28, bool, 0644);
MODULE_PARM_DESC(enable_vaud28, "set ldo-vaud28 enable bit (default 1)");

/*
 * VRF18 is fed from buck VS1.  Android runs VS1 at 2.0 V, but mainline has
 * no consumer for it, so the regulator core leaves it off at late_initcall.
 * VS1 lives at BUCK_VS1_CON0 bit 0, selector in BUCK_VS1_ELR0[6:0]
 * (1.0 V + n * 12.5 mV; 0x50 = 2.0 V).
 *
 * Writing VS1 during an mt6366_cam_ldo load reset this tablet (10:45:54,
 * right after the regmap_update_bits call), so VS1 is strictly opt-in.
 * Leave it off until a DT regulator fix makes the core own this buck.
 */
static bool enable_vs1;
module_param(enable_vs1, bool, 0644);
MODULE_PARM_DESC(enable_vs1, "enable buck VS1 at 2.0V (dangerous, default 0)");

/* Camera-rail diagnostics; only active when requested on insmod. */
static bool diag_vrf18;
module_param(diag_vrf18, bool, 0644);
MODULE_PARM_DESC(diag_vrf18, "retoggle VRF18 and test VFE28 (default 0)");

/*
 * VRF18 OP_EN (0x1c1e bit 2) is not described by the MT6358/MT6366 mainline
 * headers, and writing it resets this board's PMIC.  Keep it off unless
 * explicitly requested; CON0 bit 0 is the documented enable for this fixed
 * LDO and is what the mainline mt6358-regulator driver uses.
 */
static bool vrf18_op_en;
module_param(vrf18_op_en, bool, 0644);
MODULE_PARM_DESC(vrf18_op_en, "write undocumented VRF18 OP_EN 0x1c1e (default 0)");

/*
 * Pure-read mode: dump every camera/PMIC rail and leave all registers
 * untouched.  Used to tell a pwrap/PMIC access problem apart from a bad rail
 * write without risking another reset.
 */
static bool read_only;
module_param(read_only, bool, 0644);
MODULE_PARM_DESC(read_only, "only read PMIC state, never write (default 0)");

#define VS1_VSEL_2V0	0x50

static int vs1_mv = 2000;
module_param(vs1_mv, int, 0644);
MODULE_PARM_DESC(vs1_mv, "buck VS1 voltage in mV (default 2000)");

/*
 * VRF18 (and VFE28/VRF12) are in the "analog" LDO class: besides CON0 bit 0
 * they need their OP_EN bias bit, which lives at CON0 + 2.  The vendor
 * golden-settings table lists 0x1c1e bit 2 = 1 for VRF18 (0x1c0a bit 2 for
 * VFE28).  Mainline's mt6358-regulator never touches OP_EN, so the LDO sets
 * its enable bit but never reaches power-good on a cold PMIC.
 */
#define VRF18_OP_EN_REG		0x1c1e
#define VRF18_OP_EN_MASK	BIT(2)

static void setup_vrf18_op(struct regmap *map)
{
	unsigned int val = 0xffffffff;
	int ret;

	ret = regmap_update_bits(map, VRF18_OP_EN_REG,
				 VRF18_OP_EN_MASK, VRF18_OP_EN_MASK);
	if (regmap_read(map, VRF18_OP_EN_REG, &val))
		val = 0xffffffff;
	pr_info("mt6366_cam_ldo: VRF18 OP_EN ret=%d reg=%08x\n", ret, val);
}

static void dump_vs1(struct regmap *map)
{
	unsigned int con0 = 0xffffffff, dbg1 = 0xffffffff;
	unsigned int elr0 = 0xffffffff;
	unsigned int c2 = 0xffffffff, c3 = 0xffffffff;
	unsigned int op = 0xffffffff;

	if (regmap_read(map, MT6358_BUCK_VS1_CON0, &con0))
		con0 = 0xffffffff;
	if (regmap_read(map, MT6358_BUCK_VS1_DBG1, &dbg1))
		dbg1 = 0xffffffff;
	if (regmap_read(map, MT6358_BUCK_VS1_ELR0, &elr0))
		elr0 = 0xffffffff;

	pr_info("mt6366_cam_ldo: VS1 CON0=%08x DBG1=%08x ELR0=%08x\n",
		con0, dbg1, elr0);

	/* VRF18 has extra CON2/CON3 registers; dump them for diagnostics. */
	if (regmap_read(map, MT6358_LDO_VRF18_CON2, &c2))
		c2 = 0xffffffff;
	if (regmap_read(map, MT6358_LDO_VRF18_CON3, &c3))
		c3 = 0xffffffff;
	pr_info("mt6366_cam_ldo: VRF18 CON2=%08x CON3=%08x\n", c2, c3);

	if (regmap_read(map, VRF18_OP_EN_REG, &op))
		op = 0xffffffff;
	pr_info("mt6366_cam_ldo: VRF18 CON0+2=%08x\n", op);
}

static void setup_vs1(struct regmap *map)
{
	unsigned int con0 = 0xffffffff, dbg1 = 0xffffffff;
	unsigned int elr0 = 0xffffffff;
	unsigned int sel;
	int ret;

	if (!enable_vs1) {
		pr_info("mt6366_cam_ldo: VS1 left untouched (enable_vs1=0)\n");
		return;
	}

	if (regmap_read(map, MT6358_BUCK_VS1_CON0, &con0))
		con0 = 0xffffffff;
	if (regmap_read(map, MT6358_BUCK_VS1_ELR0, &elr0))
		elr0 = 0xffffffff;

	if (vs1_mv < 1000)
		vs1_mv = 1000;
	if (vs1_mv > 2587)
		vs1_mv = 2587;
	sel = (vs1_mv - 1000) / 12500;
	if (sel > 0x7f)
		sel = 0x7f;

	if ((elr0 & 0x7f) != sel) {
		ret = regmap_update_bits(map, MT6358_BUCK_VS1_ELR0,
					 0x7f, sel);
		pr_info("mt6366_cam_ldo: VS1 set %dmV sel=%#x ret=%d\n",
			vs1_mv, sel, ret);
	}

	if (!(con0 & BIT(0))) {
		ret = regmap_update_bits(map, MT6358_BUCK_VS1_CON0,
					 BIT(0), BIT(0));
		pr_info("mt6366_cam_ldo: VS1 enable ret=%d\n", ret);
	}

	msleep(2);

	if (regmap_read(map, MT6358_BUCK_VS1_CON0, &con0))
		con0 = 0xffffffff;
	if (regmap_read(map, MT6358_BUCK_VS1_DBG1, &dbg1))
		dbg1 = 0xffffffff;
	pr_info("mt6366_cam_ldo: VS1 after CON0=%08x DBG1=%08x ST=%s\n",
		con0, dbg1, (dbg1 & BIT(0)) ? "on" : "off");
}

static void dump_ldo(struct regmap *map, const char *name,
		     unsigned int con0, unsigned int con1,
		     unsigned int ana)
{
	unsigned int c0 = 0xffffffff, c1 = 0xffffffff, a = 0xffffffff;

	pr_info("mt6366_cam_ldo: reading %s %#06x/%#06x/%#06x\n",
		name, con0, con1, ana);
	if (regmap_read(map, con0, &c0))
		c0 = 0xffffffff;
	if (regmap_read(map, con1, &c1))
		c1 = 0xffffffff;
	if (ana && regmap_read(map, ana, &a))
		a = 0xffffffff;

	/* CON1 bit 15 is the LDO power-good / enable status (qi mask). */
	pr_info("mt6366_cam_ldo: %s CON0=%08x CON1=%08x ST=%s ANA=%08x\n",
		name, c0, c1, (c1 & BIT(15)) ? "on" : "off", a);
}

static void enable_ldo(struct regmap *map, const char *name,
		       unsigned int con0, bool do_enable)
{
	unsigned int val;
	int ret;

	if (!do_enable) {
		pr_info("mt6366_cam_ldo: %s left untouched (param off)\n",
			name);
		return;
	}

	ret = regmap_read(map, con0, &val);
	if (ret) {
		pr_err("mt6366_cam_ldo: %s read failed: %d\n", name, ret);
		return;
	}
	if (val & BIT(0)) {
		pr_info("mt6366_cam_ldo: %s already enabled\n", name);
		return;
	}

	ret = regmap_update_bits(map, con0, BIT(0), BIT(0));
	pr_info("mt6366_cam_ldo: %s enable ret=%d\n", name, ret);
}

static int __init mt6366_cam_ldo_init(void)
{
	struct device *dev;
	struct regmap *map = NULL;

	dev = bus_find_device_by_name(&platform_bus_type, NULL,
				       "10026000.pwrap");
	if (!dev) {
		pr_err("mt6366_cam_ldo: pwrap device not found\n");
		return -ENODEV;
	}

	map = dev_get_regmap(dev, NULL);
	if (!map) {
		pr_err("mt6366_cam_ldo: no regmap on %s\n", dev_name(dev));
		put_device(dev);
		return -ENODEV;
	}

	dump_ldo(map, "VRF18", MT6358_LDO_VRF18_CON0,
		 MT6358_LDO_VRF18_CON1, MT6358_VRF18_ANA_CON0);
	dump_ldo(map, "VCAMIO", MT6358_LDO_VCAMIO_CON0,
		 0x1cbe, MT6358_VCAMIO_ANA_CON0);
	dump_ldo(map, "VCAMD", MT6358_LDO_VCAMD_CON0,
		 0x1caa, 0);
	dump_ldo(map, "VCAMA1", MT6358_LDO_VCAMA1_CON0,
		 0x1c7a, 0);
	dump_ldo(map, "VCAMA2", MT6358_LDO_VCAMA2_CON0,
		 0x1c96, 0);
	dump_ldo(map, "VA12", MT6358_LDO_VA12_CON0,
		 MT6358_LDO_VA12_CON1, MT6358_VA12_ANA_CON0);
	dump_ldo(map, "VAUD28", MT6358_LDO_VAUD28_CON0,
		 MT6358_LDO_VAUD28_CON1, MT6358_VAUD28_ANA_CON0);
	dump_ldo(map, "VIO18", MT6358_LDO_VIO18_CON0,
		 MT6358_LDO_VIO18_CON1, 0);
	dump_ldo(map, "VIO28", MT6358_LDO_VIO28_CON0,
		 0x1ae6, MT6358_VIO28_ANA_CON0);
	dump_ldo(map, "VBIF28", MT6358_LDO_VBIF28_CON0,
		 0x1dac, 0);
	dump_ldo(map, "VRF12", MT6358_LDO_VRF12_CON0,
		 MT6358_LDO_VRF12_CON1, 0);
	dump_ldo(map, "VFE28", MT6358_LDO_VFE28_CON0,
		 MT6358_LDO_VFE28_CON1, 0);

	dump_vs1(map);

	if (read_only) {
		pr_info("mt6366_cam_ldo: read-only mode, no register writes\n");
		put_device(dev);
		return 0;
	}

	/* Opt-in only: writing VS1 reset this board at 10:45:54. */
	setup_vs1(map);

	if (diag_vrf18) {
		regmap_update_bits(map, MT6358_LDO_VRF18_CON0, BIT(0), 0);
		msleep(20);
		pr_info("mt6366_cam_ldo: diag: VRF18 pre-cleared\n");
	}

	if (enable_vrf18) {
		unsigned int cfg_val = 0;
		int ret;

		/* Assert HW1_OP_CFG = 1 (0x1c24 bit 2 = 0x0004).
		 * On this WiFi-only tablet SRCLKEN1 is permanently LOW.
		 * HW1_OP_CFG forces the power-good gate open so CON1 flips to 0xc100.
		 */
		ret = regmap_update_bits(map, 0x1c24, BIT(2), BIT(2));
		if (!regmap_read(map, 0x1c24, &cfg_val))
			pr_info("mt6366_cam_ldo: VRF18 OP_CFG 0x1c24=%04x ret=%d\n", cfg_val, ret);
	}

	if (enable_vrf18 && vrf18_op_en)
		setup_vrf18_op(map);
	if (disable_vrf18) {
		unsigned int val = 0xffffffff;

		regmap_update_bits(map, MT6358_LDO_VRF18_CON0, BIT(0), 0);
		regmap_read(map, MT6358_LDO_VRF18_CON0, &val);
		pr_info("mt6366_cam_ldo: VRF18 forced off CON0=%08x\n", val);
	} else {
		enable_ldo(map, "VRF18", MT6358_LDO_VRF18_CON0, enable_vrf18);
	}
	enable_ldo(map, "VA12", MT6358_LDO_VA12_CON0, enable_va12);
	enable_ldo(map, "VAUD28", MT6358_LDO_VAUD28_CON0, enable_vaud28);

	msleep(5);

	dump_ldo(map, "VRF18", MT6358_LDO_VRF18_CON0,
		 MT6358_LDO_VRF18_CON1, MT6358_VRF18_ANA_CON0);
	dump_ldo(map, "VA12", MT6358_LDO_VA12_CON0,
		 MT6358_LDO_VA12_CON1, MT6358_VA12_ANA_CON0);
	dump_ldo(map, "VAUD28", MT6358_LDO_VAUD28_CON0,
		 MT6358_LDO_VAUD28_CON1, MT6358_VAUD28_ANA_CON0);

	if (diag_vrf18) {
		int ret;

		pr_info("mt6366_cam_ldo: diag: retoggle VRF18\n");
		ret = regmap_update_bits(map, MT6358_LDO_VRF18_CON0,
					 BIT(0), 0);
		msleep(20);
		pr_info("mt6366_cam_ldo: diag: VRF18 off ret=%d\n", ret);
		dump_ldo(map, "VRF18", MT6358_LDO_VRF18_CON0,
			 MT6358_LDO_VRF18_CON1, MT6358_VRF18_ANA_CON0);
		ret = regmap_update_bits(map, MT6358_LDO_VRF18_CON0,
					 BIT(0), BIT(0));
		msleep(20);
		pr_info("mt6366_cam_ldo: diag: VRF18 on ret=%d\n", ret);
		dump_ldo(map, "VRF18", MT6358_LDO_VRF18_CON0,
			 MT6358_LDO_VRF18_CON1, MT6358_VRF18_ANA_CON0);

		pr_info("mt6366_cam_ldo: diag: enable VFE28\n");
		ret = regmap_update_bits(map, MT6358_LDO_VFE28_CON0 + 2,
					 BIT(2), BIT(2));
		pr_info("mt6366_cam_ldo: diag: VFE28 OP_EN ret=%d\n", ret);
		ret = regmap_update_bits(map, MT6358_LDO_VFE28_CON0,
					 BIT(0), BIT(0));
		msleep(20);
		pr_info("mt6366_cam_ldo: diag: VFE28 on ret=%d\n", ret);
		dump_ldo(map, "VFE28", MT6358_LDO_VFE28_CON0,
			 MT6358_LDO_VFE28_CON1, 0);
		regmap_update_bits(map, MT6358_LDO_VFE28_CON0, BIT(0), 0);
	}

	put_device(dev);
	return 0;
}

static void __exit mt6366_cam_ldo_exit(void)
{
	pr_info("mt6366_cam_ldo: unloaded, PMIC LDO state left unchanged\n");
}

module_init(mt6366_cam_ldo_init);
module_exit(mt6366_cam_ldo_exit);

MODULE_DESCRIPTION("MT6366 camera DOVDD/DVDD/AVDD rail helper for TB351FU");
MODULE_LICENSE("GPL");
