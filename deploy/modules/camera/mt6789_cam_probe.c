// SPDX-License-Identifier: GPL-2.0
/*
 * mt6789_cam_probe - standalone TB351FU camera bring-up probe
 *
 * Binds to the stock "mediatek,imgsensor" node (/kd-camera-hw1), applies the
 * exact vendor pinctrl power sequence, enables the PMIC rails, programs the
 * CAMTG MCLK muxes directly in topckgen (the mainline mt6789 camsys clock
 * provider does not bind because the stock DTB uses "mt6789-camsys_main"),
 * and then reads the sensor / AF chip IDs over I2C.
 *
 * Android proves the populated modules: the rear connector carries the
 * S5K4H7YX (i2c8, 7-bit 0x10, write ID 0x20, ID 0x487b) and the front
 * connector carries the SC820CS (i2c4, 7-bit 0x36, write ID 0x6c, ID 0xd154).
 * The probe powers both connectors and tests the plausible bus/address/ID
 * combinations rather than trusting the DT node labels.
 *
 * AF   : GT9769    7-bit 0x0E on i2c8 (DT placeholder is 0x14)
 *
 * The stock DT nodes camera_main@36 / camera_sub@10 are MTK locator clients,
 * not the sensor write addresses; sensor IDs are read directly through the
 * adapters, so no i2c client is registered and nothing can collide with the
 * DT nodes.
 *
 * Load mt6789_cam_pd.ko first: it powers CAM/CAM_RAWA/CAM_RAWB and releases
 * the CAM AXI bus protection.  This module only uses the sensor rails and
 * the CAMTG pins.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/regmap.h>

#define TOPCKGEN_PHYS_BASE	0x10000000UL
#define TOPCKGEN_MAP_SIZE	0x1000UL
#define CAMSYS_MAIN_PHYS_BASE	0x1a000000UL
#define CAMSYS_MAP_SIZE		0x1000UL
#define GPIO_PHYS_BASE		0x10005000UL
#define GPIO_MAP_SIZE		0x1000UL
#define GPIO_MODE_BASE		0x0300UL
#define GPIO_MODE_STRIDE	0x0010UL

/* CMMCLK pads: pin 128 = CMMCLK2 (rear), pin 126 = CMMCLK0 (front). */
#define REAR_MCLK_PIN		128
#define FRONT_MCLK_PIN		126

/* mt6366 ldo-vrf18 (camera DOVDD, 1.8 V fixed): enable is CON0 bit 0. */
#define PMIC_PWRAP_DEV		"10026000.pwrap"
#define MT6358_LDO_VRF18_CON0	0x1c1c

#define SPM_PHYS_BASE		0x10006000UL
#define SPM_MAP_SIZE		0x1000UL
#define SPM_PWR_STATUS		0x016c
#define SPM_CAM_ON		BIT(23)

/* topckgen mux/gate registers: status, set, clear, update */
#define TOP_CAM_SEL		0x0030
#define TOP_CAM_SEL_SET		0x0034
#define TOP_CAM_SEL_CLR		0x0038
#define TOP_CAM_GATE		BIT(23)
#define TOP_CAM_UPD_SHIFT	10

struct camtg_mux {
	const char *name;
	u32 reg;	/* sel/status register */
	u32 lsb;
	u32 width;
	u32 gate_bit;
	u32 upd_shift;
};

static const struct camtg_mux camtg_muxes[] = {
	{ "CAMTG",  0x0050, 24, 3, 31, 19 },
	{ "CAMTG2", 0x0060,  0, 3,  7, 20 },
	{ "CAMTG3", 0x0060,  8, 3, 15, 21 },
	{ "CAMTG4", 0x0060, 16, 3, 23, 22 },
	{ "CAMTG5", 0x0060, 24, 3, 31, 23 },
	{ "CAMTG6", 0x0070,  0, 3,  7, 24 },
};

#define TOP_CLK_CFG_UPDATE	0x0004
#define CAMSYS_CG_STA		0x0000
#define CAMSYS_CG_SET		0x0004
#define CAMSYS_CG_CLR		0x0008
#define CAMSYS_MAIN_CAM		BIT(6)
#define CAMSYS_MAIN_CAMTG	BIT(7)
#define CAMSYS_MAIN_SENINF	BIT(8)

/*
 * CAMTG parent mux index.  The vendor HAL ends up on index 1 (24 MHz), but
 * the board also exposes index 0 (tck_26m_mx9_ck).  Probe both when the
 * sensors do not answer.
 */
#define CAMTG_PARENT_26M	0
#define CAMTG_PARENT_24M	1

#define REAR_I2C_PATH	"/soc/i2c@11eb2000"
#define FRONT_I2C_PATH	"/soc/i2c@11eb1000"
#define REAR_I2C_ADDR	0x36
#define FRONT_I2C_ADDR	0x10
#define AF_I2C_ADDR	0x0e

/*
 * Vendor DT expresses reset and the vcamd/vcama load-switch enables as
 * pinctrl states, one pin per state.  Selecting those states one after
 * another on the same pinctrl handle makes the core release the previously
 * selected pins, so every earlier pin ends up back in GPIO-input mode.
 * Drive those pins through the GPIO API instead.  The MCLK pads need their
 * alternate function, which is also programmed directly (see
 * cam_mclk_set()) so that routing one pad never releases the other.
 */
#define CAM0_RST_GPIO		20
#define CAM0_VCAMD_GPIO		37
#define CAM0_VCAMA_GPIO		155
#define CAM1_RST_GPIO		16
#define CAM1_VCAMD_GPIO		154
#define CAM1_VCAMA_GPIO		152
#define CAM_GPIO_CHIP_LABEL	"pinctrl_paris"
#define CAM_GPIO_MAX		8

#define S5K4H7YX_ID	0x487b
#define S5K4H7YX2ST_ID	0x487c
#define SC820CS_ID	0xd154
#define SC820CS2ST_ID	0xd155

static void __iomem *topckgen;
static void __iomem *camsys;
static void __iomem *spm;
static void __iomem *gpio_base;
static struct regmap *pmic_map;

static bool do_i2c = true;
module_param(do_i2c, bool, 0644);
MODULE_PARM_DESC(do_i2c, "read sensor/AF chip IDs (default 1)");

static int camtg_parent = CAMTG_PARENT_24M;
module_param(camtg_parent, int, 0644);
MODULE_PARM_DESC(camtg_parent, "CAMTG parent mux: 0=26 MHz, 1=24 MHz (default)");

static bool keep_power;
module_param(keep_power, bool, 0644);
MODULE_PARM_DESC(keep_power, "leave sensors powered after probe (default 0)");

static bool probe_rear = true;
module_param(probe_rear, bool, 0644);
MODULE_PARM_DESC(probe_rear, "probe the rear S5K4H7YX (default 1)");

static bool probe_front = true;
module_param(probe_front, bool, 0644);
MODULE_PARM_DESC(probe_front, "probe the front SC820CS (default 1)");

static bool sweep_rails;
module_param(sweep_rails, bool, 0644);
MODULE_PARM_DESC(sweep_rails, "scan reset/rail GPIO combinations once (default 0)");

static bool dovdd_cycle = true;
module_param(dovdd_cycle, bool, 0644);
MODULE_PARM_DESC(dovdd_cycle,
		 "power-cycle ldo-vrf18 in the vendor rail order (default 1)");

/*
 * The live TB351FU DTB lists cam0 as
 *   "s5k4h7yx_mipi_raw sc820cs2st_mipi_raw"
 * and cam1 as
 *   "sc820cs_mipi_raw s5k4h7yx2st_mipi_raw".
 * Android confirms cam0 is an S5K4H7YX (ID 0x487b, write ID 0x20) and
 * cam1 probes at 7-bit 0x10 on i2c4.  The rail order still differs between
 * the two module families, so keep both selectable.
 * rear_seq/front_seq: 0 = S5K4H7 (AVDD, DVDD, DOVDD, AFVDD),
 *                     1 = SC820CS (DOVDD, DVDD, AVDD).
 */
static int rear_seq;
module_param(rear_seq, int, 0644);
MODULE_PARM_DESC(rear_seq, "rear rail order: 0=S5K4H7 (default), 1=SC820CS");

static int front_seq = 1;
module_param(front_seq, int, 0644);
MODULE_PARM_DESC(front_seq, "front rail order: 0=S5K4H7, 1=SC820CS (default)");

struct cam_ctx {
	struct device *dev;
	struct pinctrl *pctl;
	struct regulator *vcamio;
	struct regulator *vcamaf;
	/*
	 * cam0_vcamio and cam1_vcamio are the same PMIC rail (ldo-vrf18), so
	 * each sensor needs its own enable flag: a single shared flag would
	 * let one power-off path skip the regulator_disable() that balances
	 * the other sensor's regulator_enable().
	 */
	bool rear_vcamio_on;
	bool front_vcamio_on;
	bool vcamaf_on;
	bool rear_on;
	bool front_on;
};

static int cam_gpio_base = -1;
static int cam_gpios[CAM_GPIO_MAX];
static int cam_gpio_count;

/*
 * Route one CMMCLK pad to its alternate function (mode 1) without going
 * through pinctrl_select_state(): selecting cam0_mclk_* and then
 * cam1_mclk_* on the same pinctrl handle makes the core release the first
 * state's pins, so only the last routed pad would stay muxed.  The pin mode
 * registers hold one 4-bit field per pin at
 * GPIO_BASE + 0x0300 + (pin / 8) * 0x10.
 */
static void cam_mclk_set(unsigned int pin, bool on)
{
	u32 reg = GPIO_MODE_BASE + (pin / 8) * GPIO_MODE_STRIDE;
	u32 shift = (pin % 8) * 4;
	u32 val;

	if (!gpio_base)
		return;

	val = readl(gpio_base + reg);
	val &= ~(0xf << shift);
	if (on)
		val |= 1 << shift;
	writel(val, gpio_base + reg);

	pr_info("cam mclk pin %u: %s (mode %08x)\n", pin,
		on ? "CMMCLK func1" : "GPIO", readl(gpio_base + reg));
}

static int cam_gpio_claim(struct cam_ctx *c, unsigned int offset)
{
	struct gpio_device *gdev;
	int gpio, ret, i;

	if (cam_gpio_base < 0) {
		gdev = gpio_device_find_by_label(CAM_GPIO_CHIP_LABEL);
		if (!gdev) {
			dev_warn(c->dev, "GPIO chip %s not found\n",
				 CAM_GPIO_CHIP_LABEL);
			return -ENODEV;
		}
		cam_gpio_base = gpio_device_get_base(gdev);
		gpio_device_put(gdev);
	}

	gpio = cam_gpio_base + offset;
	for (i = 0; i < cam_gpio_count; i++)
		if (cam_gpios[i] == gpio)
			return gpio;

	ret = gpio_request(gpio, "cam-probe");
	if (ret) {
		dev_warn(c->dev, "gpio %d request failed: %d\n", gpio, ret);
		return ret;
	}

	ret = gpio_direction_output(gpio, 0);
	if (ret) {
		dev_warn(c->dev, "gpio %d direction failed: %d\n", gpio, ret);
		gpio_free(gpio);
		return ret;
	}

	cam_gpios[cam_gpio_count++] = gpio;
	return gpio;
}

static void cam_gpio_set(struct cam_ctx *c, unsigned int offset, int value)
{
	int gpio = cam_gpio_claim(c, offset);

	if (gpio < 0)
		return;
	gpio_set_value(gpio, value ? 1 : 0);
}

/*
 * ldo-vrf18 is the camera DOVDD rail and has no other consumers in the
 * stock DTB.  The vendor S5K4H7YX/SC820CS sequences raise DOVDD after the
 * AVDD/DVDD switches (rear) or before them (front); leaving the helper's
 * boot-time enable in place would give the sensors a DOVDD edge long before
 * the other rails.  Pulse the PMIC enable bit so the module sees a proper
 * DOVDD power-on edge.
 */
static void cam_dovdd_cycle(void)
{
	unsigned int val = 0xffffffff;
	int ret;

	if (!dovdd_cycle)
		return;

	if (!pmic_map) {
		pr_warn("cam dovdd: no PMIC regmap, leaving VRF18 as-is\n");
		return;
	}

	ret = regmap_update_bits(pmic_map, MT6358_LDO_VRF18_CON0, BIT(0), 0);
	usleep_range(2000, 3000);
	ret |= regmap_update_bits(pmic_map, MT6358_LDO_VRF18_CON0, BIT(0),
				  BIT(0));
	if (regmap_read(pmic_map, MT6358_LDO_VRF18_CON0, &val))
		val = 0xffffffff;
	pr_info("cam dovdd: VRF18 off/on ret=%d CON0=%08x\n", ret, val);
}

static void cam_reg_enable(struct cam_ctx *c, struct regulator *r,
			   bool *flag, const char *name)
{
	int ret;

	(void)flag;	/* helper-owned rails are never disabled here */

	if (!r || IS_ERR(r)) {
		dev_warn(c->dev, "regulator %s unavailable\n", name);
		return;
	}

	/*
	 * The TB351FU DTB marks these rails as non-switchable, so a direct
	 * regulator_enable() returns -EPERM and leaves the consumer enable
	 * count unbalanced (regulator_put then warns on module unload).
	 * mt6366_cam_ldo.ko owns the PMIC enable bits for VRF18/VIBR; this
	 * probe only checks that the helper already brought them up.
	 */
	ret = regulator_is_enabled(r);
	if (ret > 0) {
		dev_info(c->dev, "regulator %s already on (%d uV, helper-owned)\n",
			 name, regulator_get_voltage(r));
		return;
	}

	dev_warn(c->dev, "regulator %s is off; load mt6366_cam_ldo.ko first\n",
		 name);
}

static void cam_reg_disable(struct cam_ctx *c, struct regulator *r,
			    bool *flag, const char *name)
{
	if (!r || IS_ERR(r) || !*flag)
		return;

	regulator_disable(r);
	*flag = false;
	dev_info(c->dev, "regulator %s off\n", name);
}

static int cam_reg_set_2v8(struct cam_ctx *c, struct regulator *r,
			   const char *name)
{
	int ret;

	if (!r || IS_ERR(r))
		return -ENODEV;

	ret = regulator_set_voltage(r, 2800000, 2800000);
	if (ret && ret != -EINVAL)
		dev_warn(c->dev, "regulator %s set 2.8V failed: %d\n",
			 name, ret);

	return ret;
}

/*
 * Program every CAMTG output to the selected parent and ungate it.  We do
 * not know for certain which CAMTG the stock HAL routes to CMMCLK0/CMMCLK2,
 * so all six outputs are brought up; only the pad that the pinctrl state
 * muxes actually reaches the sensor.
 */
static int camtg_program(void)
{
	u32 freq = camtg_parent == CAMTG_PARENT_26M ? 26000000 : 24000000;
	int i;

	if (camtg_parent != CAMTG_PARENT_26M &&
	    camtg_parent != CAMTG_PARENT_24M) {
		pr_warn("invalid camtg_parent=%d, using 0 (26 MHz)\n",
			camtg_parent);
		camtg_parent = CAMTG_PARENT_26M;
		freq = 26000000;
	}

	for (i = 0; i < ARRAY_SIZE(camtg_muxes); i++) {
		const struct camtg_mux *m = &camtg_muxes[i];
		u32 mask = GENMASK(m->width - 1, 0) << m->lsb;
		u32 val = camtg_parent << m->lsb;
		u32 before = readl(topckgen + m->reg);
		u32 gate = BIT(m->gate_bit);
		u32 upd = BIT(m->upd_shift);

		/* mux: set the bits that must be 1, clear the others */
		if (val)
			writel(val, topckgen + m->reg + 0x4);
		if (mask & ~val)
			writel(mask & ~val, topckgen + m->reg + 0x8);
		if (upd)
			writel(upd, topckgen + TOP_CLK_CFG_UPDATE);
		udelay(2);

		/* gate on: active-low, enable by writing CLR */
		writel(gate, topckgen + m->reg + 0x8);
		udelay(1);
		pr_info("topckgen %s: %08x -> %08x (parent %d, %u Hz, gate %u)\n",
			m->name, before, readl(topckgen + m->reg),
			camtg_parent, freq,
			!!(readl(topckgen + m->reg) & gate));
	}

	/* Also make sure the cam_sel parent gate is on (active-low). */
	if (readl(topckgen + TOP_CAM_SEL) & TOP_CAM_GATE) {
		writel(TOP_CAM_GATE, topckgen + TOP_CAM_SEL_CLR);
		writel(BIT(TOP_CAM_UPD_SHIFT), topckgen + TOP_CLK_CFG_UPDATE);
		udelay(1);
		pr_info("topckgen cam_sel gate enabled: %08x\n",
			readl(topckgen + TOP_CAM_SEL));
	}

	/* camsys CAMTG/CAM/SENINF gates: active-low, enable via CLR */
	pr_info("camsys gates before: %08x\n", readl(camsys + CAMSYS_CG_STA));
	writel(CAMSYS_MAIN_CAM | CAMSYS_MAIN_CAMTG | CAMSYS_MAIN_SENINF,
	       camsys + CAMSYS_CG_CLR);
	udelay(1);
	pr_info("camsys gates after:  %08x\n", readl(camsys + CAMSYS_CG_STA));

	return 0;
}

/*
 * Two-message I2C register read.  reg16 selects a big-endian 16-bit register
 * address (Samsung/SmartSens sensors) instead of an 8-bit one (VCM).
 */
static int cam_i2c_read(struct i2c_adapter *adap, u16 addr, u16 reg,
			u8 *buf, u16 len, bool reg16)
{
	struct i2c_msg msgs[2];
	u8 ra[2] = { reg >> 8, reg & 0xff };
	int ret;

	msgs[0].addr = addr;
	msgs[0].flags = 0;
	msgs[0].len = reg16 ? 2 : 1;
	msgs[0].buf = reg16 ? ra : &ra[1];

	msgs[1].addr = addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	ret = i2c_transfer(adap, msgs, 2);
	if (ret < 0)
		return ret;
	return ret == 2 ? 0 : -EIO;
}

static struct i2c_adapter *cam_get_adapter(const char *path)
{
	struct device_node *np;
	struct i2c_adapter *adap;

	np = of_find_node_by_path(path);
	if (!np) {
		pr_err("i2c node %s not found\n", path);
		return NULL;
	}

	adap = of_find_i2c_adapter_by_node(np);
	of_node_put(np);
	if (!adap)
		pr_err("no i2c adapter for %s (driver not bound?)\n", path);

	return adap;
}

static bool scan_buses;
module_param(scan_buses, bool, 0644);
MODULE_PARM_DESC(scan_buses, "scan plausible sensor addresses on both i2c buses");

static void cam_scan_bus(struct device *dev, const char *path)
{
	static const u16 addrs[] = { 0x10, 0x36, 0x50, 0x51, 0x0e, 0x14 };
	struct i2c_adapter *adap;
	u8 b[2];
	int i, ret;

	adap = cam_get_adapter(path);
	if (!adap)
		return;

	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		ret = cam_i2c_read(adap, addrs[i], 0x0000, b, 2, true);
		dev_info(dev, "scan %s @0x%02x reg0000: ret=%d val=%02x%02x\n",
			 path, addrs[i], ret, ret ? 0 : b[0], ret ? 0 : b[1]);
		ret = cam_i2c_read(adap, addrs[i], 0x3107, b, 2, true);
		dev_info(dev, "scan %s @0x%02x reg3107: ret=%d val=%02x%02x\n",
			 path, addrs[i], ret, ret ? 0 : b[0], ret ? 0 : b[1]);
		ret = cam_i2c_read(adap, addrs[i], 0x00, b, 1, false);
		dev_info(dev, "scan %s @0x%02x reg00:   ret=%d val=%02x\n",
			 path, addrs[i], ret, ret ? 0 : b[0]);
	}

	i2c_put_adapter(adap);
}

static void cam_probe_addr(struct device *dev, struct i2c_adapter *adap,
			   const char *bus, u16 addr)
{
	u8 s5k[2] = { 0, 0 };
	u8 sc_hi = 0, sc_lo = 0;
	u8 v = 0;
	u32 id;
	int ret;

	/* Samsung S5K4H7YX / 2ST: 16-bit register, 8-bit data.  The vendor
	 * driver reads 0x0000 and 0x0001 as two separate byte reads; mirror
	 * that exactly so a sensor that does not auto-increment still works.
	 */
	ret = cam_i2c_read(adap, addr, 0x0000, &s5k[0], 1, true);
	if (!ret)
		ret = cam_i2c_read(adap, addr, 0x0001, &s5k[1], 1, true);
	if (ret) {
		dev_info(dev, "%s @0x%02x S5K4H7 reg0000: ret=%d\n",
			 bus, addr, ret);
	} else {
		id = (s5k[0] << 8) | s5k[1];
		dev_info(dev, "%s @0x%02x S5K4H7 reg0000: id=0x%04x%s\n",
			 bus, addr, id,
			 (id == S5K4H7YX_ID || id == S5K4H7YX2ST_ID) ?
			 " MATCH" : "");
	}

	/* SmartSens SC820CS / 2ST: 16-bit registers, 8-bit data */
	ret = cam_i2c_read(adap, addr, 0x3107, &sc_hi, 1, true);
	if (!ret)
		ret = cam_i2c_read(adap, addr, 0x3108, &sc_lo, 1, true);
	if (ret) {
		dev_info(dev, "%s @0x%02x SC820CS reg3107/8: ret=%d\n",
			 bus, addr, ret);
	} else {
		id = (sc_hi << 8) | sc_lo;
		dev_info(dev, "%s @0x%02x SC820CS reg3107/8: id=0x%04x%s\n",
			 bus, addr, id,
			 (id == SC820CS_ID || id == SC820CS2ST_ID) ?
			 " MATCH" : "");
	}

	/*
	 * Both vendor SC820CS drivers (0x6c write ID, 7-bit 0x36) read their
	 * ID from 0x3107/0x3108 above; there is no alternate ID pair.
	 */

	/* Plain 8-bit register read identifies VCM/EEPROM devices. */
	ret = cam_i2c_read(adap, addr, 0x00, &v, 1, false);
	dev_info(dev, "%s @0x%02x reg00: ret=%d val=%02x\n",
		 bus, addr, ret, ret ? 0 : v);
}

/*
 * Android proved the mapping: the rear connector carries the S5K4H7YX at
 * 7-bit 0x10 (write ID 0x20, ID 0x487b) and the front connector carries the
 * SC820CS at 7-bit 0x36 (write ID 0x6c, ID 0xd154).  The DT node addresses
 * camera_main@36 / camera_sub@10 are MTK locator clients, so both sensor
 * addresses are still tried on both buses; 0x50/0x51 are the two EEPROMs.
 */
static void cam_probe_id_matrix(struct device *dev)
{
	static const struct {
		const char *path;
		const char *bus;
	} buses[] = {
		{ REAR_I2C_PATH,  "i2c8" },
		{ FRONT_I2C_PATH, "i2c4" },
	};
	static const u16 addrs[] = { 0x10, 0x36, 0x50, 0x51 };
	struct i2c_adapter *adap;
	int b, a;

	for (b = 0; b < ARRAY_SIZE(buses); b++) {
		adap = cam_get_adapter(buses[b].path);
		if (!adap)
			continue;

		for (a = 0; a < ARRAY_SIZE(addrs); a++)
			cam_probe_addr(dev, adap, buses[b].bus, addrs[a]);

		i2c_put_adapter(adap);
	}
}

static void cam_read_af(struct device *dev)
{
	struct i2c_adapter *adap;
	u8 v = 0;
	int ret;

	adap = cam_get_adapter(REAR_I2C_PATH);
	if (!adap)
		return;

	ret = cam_i2c_read(adap, AF_I2C_ADDR, 0x00, &v, 1, false);
	if (ret)
		dev_err(dev, "GT9769 AF @0x%02x: not responding (%d)\n",
			AF_I2C_ADDR, ret);
	else
		dev_info(dev, "GT9769 AF @0x%02x: present, reg0=0x%02x\n",
			 AF_I2C_ADDR, v);

	i2c_put_adapter(adap);
}

/*
 * Read an ID stored as two 8-bit registers with 16-bit register addresses.
 * Returns 0 only when both transfers were ACKed.
 */
static int cam_read_id_pair(struct i2c_adapter *adap, u16 addr, u16 reg,
			    u8 *hi, u8 *lo)
{
	int ret;

	*hi = 0;
	*lo = 0;
	ret = cam_i2c_read(adap, addr, reg, hi, 1, true);
	if (ret)
		return ret;
	return cam_i2c_read(adap, addr, reg + 1, lo, 1, true);
}

/*
 * Drive every reset/load-switch GPIO through all 64 combinations and report
 * any combination in which a sensor acknowledges its ID registers.  This
 * distinguishes a wrong rail polarity or swapped GPIO mapping from a
 * missing clock or a dead module without needing a meter.
 */
static void cam_rail_sweep(struct cam_ctx *c)
{
	static const unsigned int pins[] = {
		CAM0_RST_GPIO, CAM0_VCAMD_GPIO, CAM0_VCAMA_GPIO,
		CAM1_RST_GPIO, CAM1_VCAMD_GPIO, CAM1_VCAMA_GPIO,
	};
	struct i2c_adapter *rear, *front;
	u8 hi, lo;
	int i, k, hits = 0;

	rear = cam_get_adapter(REAR_I2C_PATH);
	front = cam_get_adapter(FRONT_I2C_PATH);

	/* Keep both MCLK pads routed while the rails are swept. */
	cam_mclk_set(REAR_MCLK_PIN, true);
	cam_mclk_set(FRONT_MCLK_PIN, true);

	for (i = 0; i < 64; i++) {
		for (k = 0; k < ARRAY_SIZE(pins); k++)
			cam_gpio_set(c, pins[k], !!(i & BIT(k)));
		msleep(25);

		if (rear && !cam_read_id_pair(rear, 0x10, 0x0000, &hi, &lo)) {
			dev_info(c->dev, "sweep %02x rear 0x10 ACK id=%02x%02x\n",
				 i, hi, lo);
			hits++;
		}
		if (front && !cam_read_id_pair(front, 0x36, 0x3107, &hi, &lo)) {
			dev_info(c->dev, "sweep %02x front 0x36 ACK id=%02x%02x\n",
				 i, hi, lo);
			hits++;
		}
	}

	dev_info(c->dev, "rail sweep done: %d ACK(s) over 64 combinations\n",
		 hits);

	if (rear)
		i2c_put_adapter(rear);
	if (front)
		i2c_put_adapter(front);

	/* Restore the fully powered state for the normal probe. */
	for (k = 0; k < ARRAY_SIZE(pins); k++)
		cam_gpio_set(c, pins[k], 1);
	msleep(10);
}

static int rear_power_on(struct cam_ctx *c)
{
	bool sc820cs = rear_seq != 0;

	dev_info(c->dev, "--- cam0 power sequence (%s candidate) ---\n",
		 sc820cs ? "SC820CS" : "S5K4H7YX");

	/* Vendor order both variants: reset low, MCLK on, then rails,
	 * finally release reset.  The two module families differ in the
	 * rail order and whether AFVDD is part of the sequence.
	 * Reset and rail enables are plain GPIOs; only MCLK uses pinctrl.
	 */
	c->rear_on = true;
	/* Claim reset and both rail switches first, all driven low.  The
	 * gpiolib request path reprograms the pin mux, so the MCLK state must
	 * be selected after the claims and before the rails come up.
	 */
	cam_gpio_set(c, CAM0_RST_GPIO, 0);
	cam_gpio_set(c, CAM0_VCAMD_GPIO, 0);
	cam_gpio_set(c, CAM0_VCAMA_GPIO, 0);
	cam_mclk_set(REAR_MCLK_PIN, false);
	msleep(1);

	cam_mclk_set(REAR_MCLK_PIN, true);
	udelay(200);

	if (sc820cs) {
		/* Vendor SC820CS: vcamio, vcamd, vcama; no vcamaf. */
		cam_dovdd_cycle();			/* DOVDD 1.8V edge */
		msleep(1);
		cam_gpio_set(c, CAM0_VCAMD_GPIO, 1);	/* DVDD 1.2V */
		msleep(1);
		cam_gpio_set(c, CAM0_VCAMA_GPIO, 1);	/* AVDD 2.8V */
		msleep(1);
	} else {
		/* Vendor S5K4H7: vcama, vcamd, vcamio, vcamaf. */
		cam_gpio_set(c, CAM0_VCAMA_GPIO, 1);	/* AVDD 2.8V */
		msleep(1);
		cam_gpio_set(c, CAM0_VCAMD_GPIO, 1);	/* DVDD 1.2V */
		msleep(1);
		cam_dovdd_cycle();			/* DOVDD 1.8V edge */
		msleep(1);
		cam_reg_set_2v8(c, c->vcamaf, "cam0_vcamaf");
		cam_reg_enable(c, c->vcamaf, &c->vcamaf_on,
			       "cam0_vcamaf");		/* AFVDD */
		msleep(1);
	}

	cam_gpio_set(c, CAM0_RST_GPIO, 1);
	msleep(50);

	return 0;
}

static int front_power_on(struct cam_ctx *c)
{
	bool sc820cs = front_seq != 0;

	dev_info(c->dev, "--- cam1 power sequence (%s candidate) ---\n",
		 sc820cs ? "SC820CS" : "S5K4H7YX");

	c->front_on = true;
	cam_gpio_set(c, CAM1_RST_GPIO, 0);
	cam_gpio_set(c, CAM1_VCAMD_GPIO, 0);
	cam_gpio_set(c, CAM1_VCAMA_GPIO, 0);
	cam_mclk_set(FRONT_MCLK_PIN, false);
	msleep(1);

	cam_mclk_set(FRONT_MCLK_PIN, true);
	udelay(200);

	if (sc820cs) {
		/* SC820CS: vcamio, vcamd, vcama. */
		cam_dovdd_cycle();			/* DOVDD 1.8V edge */
		msleep(1);
		cam_gpio_set(c, CAM1_VCAMD_GPIO, 1);	/* DVDD 1.2V */
		msleep(1);
		cam_gpio_set(c, CAM1_VCAMA_GPIO, 1);	/* AVDD 2.8V */
		msleep(1);
	} else {
		/* S5K4H7: vcama, vcamd, vcamio; cam1 has no AFVDD. */
		cam_gpio_set(c, CAM1_VCAMA_GPIO, 1);	/* AVDD 2.8V */
		msleep(1);
		cam_gpio_set(c, CAM1_VCAMD_GPIO, 1);	/* DVDD 1.2V */
		msleep(1);
		cam_dovdd_cycle();			/* DOVDD 1.8V edge */
		msleep(1);
	}

	cam_gpio_set(c, CAM1_RST_GPIO, 1);
	msleep(20);

	return 0;
}

static void rear_power_off(struct cam_ctx *c)
{
	if (!c->rear_on)
		return;

	cam_gpio_set(c, CAM0_RST_GPIO, 0);
	cam_mclk_set(REAR_MCLK_PIN, false);
	cam_gpio_set(c, CAM0_VCAMA_GPIO, 0);
	cam_gpio_set(c, CAM0_VCAMD_GPIO, 0);
	cam_reg_disable(c, c->vcamaf, &c->vcamaf_on, "cam0_vcamaf");
	cam_reg_disable(c, c->vcamio, &c->rear_vcamio_on, "cam0_vcamio");
	c->rear_on = false;
}

static void front_power_off(struct cam_ctx *c)
{
	if (!c->front_on)
		return;

	cam_gpio_set(c, CAM1_RST_GPIO, 0);
	cam_mclk_set(FRONT_MCLK_PIN, false);
	cam_gpio_set(c, CAM1_VCAMA_GPIO, 0);
	cam_gpio_set(c, CAM1_VCAMD_GPIO, 0);
	cam_reg_disable(c, c->vcamio, &c->front_vcamio_on, "cam1_vcamio");
	c->front_on = false;
}

static int mt6789_cam_probe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cam_ctx c = { .dev = dev };
	int ret = 0;

	dev_info(dev, "TB351FU camera probe starting\n");

	c.pctl = devm_pinctrl_get(dev);
	if (IS_ERR(c.pctl)) {
		dev_err(dev, "devm_pinctrl_get failed: %ld\n",
			PTR_ERR(c.pctl));
		return PTR_ERR(c.pctl);
	}

	c.vcamio = devm_regulator_get_optional(dev, "cam0_vcamio");
	if (IS_ERR(c.vcamio)) {
		dev_warn(dev, "cam0_vcamio supply: %ld\n", PTR_ERR(c.vcamio));
		c.vcamio = NULL;
	}
	c.vcamaf = devm_regulator_get_optional(dev, "cam0_vcamaf");
	if (IS_ERR(c.vcamaf)) {
		dev_warn(dev, "cam0_vcamaf supply: %ld\n", PTR_ERR(c.vcamaf));
		c.vcamaf = NULL;
	}

	/*
	 * ldo-vrf18 is camera DOVDD and must be pulsed in the vendor rail
	 * order; the mainline regulator cannot enable it (dummy parent), so
	 * drive the PMIC enable bit directly like mt6366_cam_ldo.ko does.
	 */
	{
		struct device *pwr = bus_find_device_by_name(&platform_bus_type,
							     NULL,
							     PMIC_PWRAP_DEV);

		if (pwr) {
			pmic_map = dev_get_regmap(pwr, NULL);
			put_device(pwr);
		}
		if (!pmic_map)
			dev_warn(dev, "PMIC regmap not found; DOVDD cycle disabled\n");
	}

	spm = ioremap(SPM_PHYS_BASE, SPM_MAP_SIZE);
	if (!spm) {
		dev_err(dev, "ioremap SPM failed\n");
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * The camsys register block and the CAMTG pads are inside the CAM
	 * power domain.  Touching them while the domain is off can hang the
	 * interconnect, so refuse to continue until mt6789_cam_pd.ko has
	 * powered CAM/CAM_RAWA/CAM_RAWB.
	 */
	if (!(readl(spm + SPM_PWR_STATUS) & SPM_CAM_ON)) {
		dev_err(dev, "CAM power domain is off (SPM status=%08x); load mt6789_cam_pd.ko first\n",
			readl(spm + SPM_PWR_STATUS));
		ret = -ENODEV;
		goto out;
	}

	topckgen = ioremap(TOPCKGEN_PHYS_BASE, TOPCKGEN_MAP_SIZE);
	camsys = ioremap(CAMSYS_MAIN_PHYS_BASE, CAMSYS_MAP_SIZE);
	gpio_base = ioremap(GPIO_PHYS_BASE, GPIO_MAP_SIZE);
	if (!topckgen || !camsys || !gpio_base) {
		dev_err(dev, "ioremap topckgen/camsys/gpio failed\n");
		ret = -ENOMEM;
		goto out;
	}

	dev_info(dev, "topckgen cam_sel=%08x camtg=%08x camsys gates=%08x\n",
		 readl(topckgen + TOP_CAM_SEL), readl(topckgen + 0x0050),
		 readl(camsys + CAMSYS_CG_STA));

	/* Program the shared CAMTG muxes once; cam_mclk_set() then routes
	 * CMMCLK0 (front) / CMMCLK2 (rear) on the respective pads.
	 */
	camtg_program();

	if (sweep_rails)
		cam_rail_sweep(&c);

	if (probe_rear)
		ret = rear_power_on(&c);
	if (!ret && probe_front)
		ret = front_power_on(&c);

	if (!ret && do_i2c) {
		msleep(30);
		if (scan_buses) {
			cam_scan_bus(dev, REAR_I2C_PATH);
			cam_scan_bus(dev, FRONT_I2C_PATH);
		}
		cam_probe_id_matrix(dev);
		if (probe_rear)
			cam_read_af(dev);
	}

	if (!keep_power) {
		front_power_off(&c);
		rear_power_off(&c);
	}

	dev_info(dev, "TB351FU camera probe done (ret=%d)\n", ret);

out:
	if (topckgen) {
		iounmap(topckgen);
		topckgen = NULL;
	}
	if (camsys) {
		iounmap(camsys);
		camsys = NULL;
	}
	if (spm) {
		iounmap(spm);
		spm = NULL;
	}
	if (gpio_base) {
		iounmap(gpio_base);
		gpio_base = NULL;
	}
	return ret;
}

static void mt6789_cam_probe_remove(struct platform_device *pdev)
{
	int i;

	cam_mclk_set(REAR_MCLK_PIN, false);
	cam_mclk_set(FRONT_MCLK_PIN, false);

	/* Drive every claimed rail/reset low, then release the lines so the
	 * next load starts from a clean, unpowered state.
	 */
	for (i = 0; i < cam_gpio_count; i++) {
		gpio_set_value(cam_gpios[i], 0);
		gpio_free(cam_gpios[i]);
	}
	cam_gpio_count = 0;
	cam_gpio_base = -1;
}

static const struct of_device_id mt6789_cam_probe_of_match[] = {
	{ .compatible = "mediatek,imgsensor" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6789_cam_probe_of_match);

static struct platform_driver mt6789_cam_probe_driver = {
	.probe = mt6789_cam_probe_probe,
	.remove = mt6789_cam_probe_remove,
	.driver = {
		.name = "mt6789-cam-probe",
		.of_match_table = mt6789_cam_probe_of_match,
	},
};
module_platform_driver(mt6789_cam_probe_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB351FU camera power/MCLK/I2C bring-up probe");
MODULE_AUTHOR("PMOS TB351FU porting");
