// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2019 Intel Corporation

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define ASPEED_SGPIO_CTRL		0x54
#define  ASPEED_SGPIO_CLK_DIV_MASK	GENMASK(31, 16)
#define   ASPEED_SGPIO_CLK_DIV_MIN	1
#define   ASPEED_SGPIO_CLK_DIV_MAX	65535
#define  ASPEED_SGPIO_PINBYTES_MASK	GENMASK(9, 6)
#define   ASPEED_SGPIO_PINBYTES_MIN	1
#define   ASPEED_SGPIO_PINBYTES_MAX	10
#define  ASPEED_SGPIO_ENABLE		BIT(0)

#define ASPEED_SGPIO_BUS_FREQ_DEFAULT	1000000

struct aspeed_bank_props {
	unsigned int bank;
	u32 input;
	u32 output;
};

struct aspeed_sgpio_config {
	unsigned int nr_pgpios;
	unsigned int nr_gpios;
	const struct aspeed_bank_props *props;
};

struct aspeed_sgpio {
	struct gpio_chip chip;
	struct irq_chip irqc;
	spinlock_t lock;
	void __iomem *base;
	int irq;
	const struct aspeed_sgpio_config *config;
};

struct aspeed_sgpio_bank {
	uint16_t	val_reg;
	uint16_t	rdata_reg;
	uint16_t	tolerance_reg;
	uint16_t	irq_regs;
	bool		support_irq;
	const char	names[4][3];
};

/*
 * Note: The "val" register returns the input value sampled on the line.
 *       Or, it can be used for writing a value on the line.
 *
 *       The "rdata" register returns the content of the write latch and thus
 *       can be used to read back what was last written reliably.
 */

static const struct aspeed_sgpio_bank aspeed_sgpio_banks[] = {
	{
		.val_reg = 0x0000,
		.rdata_reg = 0x0070,
		.tolerance_reg = 0x0018,
		.irq_regs = 0x0004,
		.support_irq = false,
		.names = { "OA", "OB", "OC", "OD" },
	},
	{
		.val_reg = 0x001C,
		.rdata_reg = 0x0074,
		.tolerance_reg = 0x0034,
		.irq_regs = 0x0020,
		.support_irq = false,
		.names = { "OE", "OF", "OG", "OH" },
	},
	{
		.val_reg = 0x0038,
		.rdata_reg = 0x0078,
		.tolerance_reg = 0x0050,
		.irq_regs = 0x003C,
		.support_irq = false,
		.names = { "OI", "OJ" },
	},
	{
		.val_reg = 0x0000,
		.rdata_reg = 0x0070,
		.tolerance_reg = 0x0018,
		.irq_regs = 0x0004,
		.support_irq = true,
		.names = { "IA", "IB", "IC", "ID" },
	},
	{
		.val_reg = 0x001C,
		.rdata_reg = 0x0074,
		.tolerance_reg = 0x0034,
		.irq_regs = 0x0020,
		.support_irq = true,
		.names = { "IE", "IF", "IG", "IH" },
	},
	{
		.val_reg = 0x0038,
		.rdata_reg = 0x0078,
		.tolerance_reg = 0x0050,
		.irq_regs = 0x003C,
		.support_irq = true,
		.names = { "II", "IJ" },
	},
};

enum aspeed_sgpio_reg {
	reg_val,
	reg_rdata,
	reg_irq_enable,
	reg_irq_type0,
	reg_irq_type1,
	reg_irq_type2,
	reg_irq_status,
	reg_tolerance,
};

#define GPIO_IRQ_ENABLE	0x00
#define GPIO_IRQ_TYPE0	0x04
#define GPIO_IRQ_TYPE1	0x08
#define GPIO_IRQ_TYPE2	0x0c
#define GPIO_IRQ_STATUS	0x10

/* This will be resolved at compile time */
static inline void __iomem *bank_reg(struct aspeed_sgpio *gpio,
				     const struct aspeed_sgpio_bank *bank,
				     const enum aspeed_sgpio_reg reg)
{
	switch (reg) {
	case reg_val:
		return gpio->base + bank->val_reg;
	case reg_rdata:
		return gpio->base + bank->rdata_reg;
	case reg_irq_enable:
		return gpio->base + bank->irq_regs + GPIO_IRQ_ENABLE;
	case reg_irq_type0:
		return gpio->base + bank->irq_regs + GPIO_IRQ_TYPE0;
	case reg_irq_type1:
		return gpio->base + bank->irq_regs + GPIO_IRQ_TYPE1;
	case reg_irq_type2:
		return gpio->base + bank->irq_regs + GPIO_IRQ_TYPE2;
	case reg_irq_status:
		return gpio->base + bank->irq_regs + GPIO_IRQ_STATUS;
	case reg_tolerance:
		return gpio->base + bank->tolerance_reg;
	default:
		WARN_ON(1);
	}

	return NULL;
}

#define GPIO_BANK(x)	((x) >> 5)
#define GPIO_OFFSET(x)	((x) & 0x1f)
#define GPIO_BIT(x)	BIT(GPIO_OFFSET(x))

static const struct aspeed_sgpio_bank *to_bank(unsigned int offset)
{
	unsigned int bank = GPIO_BANK(offset);

	WARN_ON(bank >= ARRAY_SIZE(aspeed_sgpio_banks));
	return &aspeed_sgpio_banks[bank];
}

static inline bool is_bank_props_sentinel(const struct aspeed_bank_props *props)
{
	return !(props->input || props->output);
}

static inline const struct aspeed_bank_props *find_bank_props(
		struct aspeed_sgpio *gpio, unsigned int offset)
{
	const struct aspeed_bank_props *props = gpio->config->props;

	while (!is_bank_props_sentinel(props)) {
		if (props->bank == GPIO_BANK(offset))
			return props;
		props++;
	}

	return NULL;
}

static inline bool have_input(struct aspeed_sgpio *gpio, unsigned int offset)
{
	const struct aspeed_bank_props *props = find_bank_props(gpio, offset);

	return !props || (props->input & GPIO_BIT(offset));
}

static inline bool have_output(struct aspeed_sgpio *gpio, unsigned int offset)
{
	const struct aspeed_bank_props *props = find_bank_props(gpio, offset);

	return !props || (props->output & GPIO_BIT(offset));
}

static int aspeed_sgpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct aspeed_sgpio *gpio = gpiochip_get_data(gc);
	const struct aspeed_sgpio_bank *bank = to_bank(offset);
	enum aspeed_sgpio_reg reg;

	if (have_output(gpio, offset))
		reg = reg_rdata;
	else
		reg = reg_val;

	return !!(ioread32(bank_reg(gpio, bank, reg)) & GPIO_BIT(offset));
}

static void aspeed_sgpio_set(struct gpio_chip *gc, unsigned int offset, int val)
{
	const struct aspeed_sgpio_bank *bank = to_bank(offset);
	struct aspeed_sgpio *gpio = gpiochip_get_data(gc);
	unsigned long flags;
	u32 reg;

	if (!have_output(gpio, offset))
		return;

	spin_lock_irqsave(&gpio->lock, flags);

	reg = ioread32(bank_reg(gpio, bank, reg_rdata));

	if (val)
		reg |= GPIO_BIT(offset);
	else
		reg &= ~GPIO_BIT(offset);

	iowrite32(reg, bank_reg(gpio, bank, reg_val));

	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int aspeed_sgpio_dir_in(struct gpio_chip *gc, unsigned int offset)
{
	struct aspeed_sgpio *gpio = gpiochip_get_data(gc);

	if (!have_input(gpio, offset))
		return -ENOTSUPP;

	return 0;
}

static int aspeed_sgpio_dir_out(struct gpio_chip *gc, unsigned int offset,
				int val)
{
	struct aspeed_sgpio *gpio = gpiochip_get_data(gc);

	if (!have_output(gpio, offset))
		return -ENOTSUPP;

	aspeed_sgpio_set(gc, offset, val);

	return 0;
}

static int aspeed_sgpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct aspeed_sgpio *gpio = gpiochip_get_data(gc);

	if (have_output(gpio, offset))
		return 0;
	else if (have_input(gpio, offset))
		return 1;

	return -ENOTSUPP;
}

static inline int
irqd_to_aspeed_sgpio_data(struct irq_data *d, struct aspeed_sgpio **gpio,
			  const struct aspeed_sgpio_bank **bank,
			  u32 *bit, int *offset)
{
	struct aspeed_sgpio *internal;

	*offset = irqd_to_hwirq(d);

	internal = irq_data_get_irq_chip_data(d);

	*gpio = internal;
	*bank = to_bank(*offset);
	*bit = GPIO_BIT(*offset);

	return 0;
}

static void aspeed_sgpio_irq_ack(struct irq_data *d)
{
	const struct aspeed_sgpio_bank *bank;
	struct aspeed_sgpio *gpio;
	void __iomem *status_addr;
	unsigned long flags;
	int rc, offset;
	u32 bit;

	rc = irqd_to_aspeed_sgpio_data(d, &gpio, &bank, &bit, &offset);
	if (rc)
		return;

	status_addr = bank_reg(gpio, bank, reg_irq_status);

	spin_lock_irqsave(&gpio->lock, flags);

	iowrite32(bit, status_addr);

	spin_unlock_irqrestore(&gpio->lock, flags);
}

static void aspeed_sgpio_irq_set_mask(struct irq_data *d, bool set)
{
	const struct aspeed_sgpio_bank *bank;
	struct aspeed_sgpio *gpio;
	unsigned long flags;
	u32 reg, bit;
	void __iomem *addr;
	int rc, offset;

	rc = irqd_to_aspeed_sgpio_data(d, &gpio, &bank, &bit, &offset);
	if (rc)
		return;

	if (!bank->support_irq)
		return;

	addr = bank_reg(gpio, bank, reg_irq_enable);

	spin_lock_irqsave(&gpio->lock, flags);

	reg = ioread32(addr);
	if (set)
		reg |= bit;
	else
		reg &= ~bit;

	iowrite32(reg, addr);

	spin_unlock_irqrestore(&gpio->lock, flags);
}

static void aspeed_sgpio_irq_mask(struct irq_data *d)
{
	aspeed_sgpio_irq_set_mask(d, false);
}

static void aspeed_sgpio_irq_unmask(struct irq_data *d)
{
	aspeed_sgpio_irq_set_mask(d, true);
}

static int aspeed_sgpio_set_type(struct irq_data *d, unsigned int type)
{
	u32 type0 = 0;
	u32 type1 = 0;
	u32 type2 = 0;
	u32 bit, reg;
	const struct aspeed_sgpio_bank *bank;
	irq_flow_handler_t handler;
	struct aspeed_sgpio *gpio;
	unsigned long flags;
	void __iomem *addr;
	int rc, offset;

	rc = irqd_to_aspeed_sgpio_data(d, &gpio, &bank, &bit, &offset);
	if (rc)
		return -EINVAL;

	if (!bank->support_irq)
		return -ENOTSUPP;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_BOTH:
		type2 |= bit;
		/* fall through */
	case IRQ_TYPE_EDGE_RISING:
		type0 |= bit;
		/* fall through */
	case IRQ_TYPE_EDGE_FALLING:
		handler = handle_edge_irq;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		type0 |= bit;
		/* fall through */
	case IRQ_TYPE_LEVEL_LOW:
		type1 |= bit;
		handler = handle_level_irq;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&gpio->lock, flags);

	addr = bank_reg(gpio, bank, reg_irq_type0);
	reg = ioread32(addr);
	reg = (reg & ~bit) | type0;
	iowrite32(reg, addr);

	addr = bank_reg(gpio, bank, reg_irq_type1);
	reg = ioread32(addr);
	reg = (reg & ~bit) | type1;
	iowrite32(reg, addr);

	addr = bank_reg(gpio, bank, reg_irq_type2);
	reg = ioread32(addr);
	reg = (reg & ~bit) | type2;
	iowrite32(reg, addr);

	spin_unlock_irqrestore(&gpio->lock, flags);

	irq_set_handler_locked(d, handler);

	return 0;
}

static void aspeed_sgpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct aspeed_sgpio *data = gpiochip_get_data(gc);
	struct irq_chip *ic = irq_desc_get_chip(desc);
	unsigned int i, p, girq;
	unsigned long reg;

	chained_irq_enter(ic, desc);

	for (i = 0; i < ARRAY_SIZE(aspeed_sgpio_banks); i++) {
		const struct aspeed_sgpio_bank *bank = &aspeed_sgpio_banks[i];

		if (!bank->support_irq)
			continue;

		reg = ioread32(bank_reg(data, bank, reg_irq_status));

		for_each_set_bit(p, &reg, 32) {
			girq = irq_find_mapping(gc->irq.domain, i * 32 + p);
			generic_handle_irq(girq);
		}
	}

	chained_irq_exit(ic, desc);
}

static void aspeed_sgpio_init_irq_valid_mask(struct gpio_chip *gc,
					     unsigned long *valid_mask,
					     unsigned int ngpios)
{
	struct aspeed_sgpio *gpio = gpiochip_get_data(gc);
	const struct aspeed_bank_props *props = gpio->config->props;

	while (!is_bank_props_sentinel(props)) {
		unsigned int offset;
		const unsigned long int input = props->input;

		/* Pretty crummy approach, but similar to GPIO core */
		for_each_clear_bit(offset, &input, 32) {
			unsigned int i = props->bank * 32 + offset;

			if (i >= gpio->chip.ngpio)
				break;

			clear_bit(i, valid_mask);
		}

		props++;
	}
}

static int aspeed_sgpio_setup_irqs(struct aspeed_sgpio *gpio,
				   struct platform_device *pdev)
{
	const struct aspeed_sgpio_bank *bank;
	struct gpio_irq_chip *girq;
	int rc, i;

	/* Initialize IRQ and tolerant settings */
	for (i = 0; i < ARRAY_SIZE(aspeed_sgpio_banks); i++) {
		bank =  &aspeed_sgpio_banks[i];

		/* Value will be reset by WDT reset */
		iowrite32(0x00000000, bank_reg(gpio, bank, reg_tolerance));

		if (!bank->support_irq)
			continue;

		/* disable irq enable bits */
		iowrite32(0x00000000, bank_reg(gpio, bank, reg_irq_enable));
		/* clear status bits */
		iowrite32(0xffffffff, bank_reg(gpio, bank, reg_irq_status));
		/* set rising or level-high irq */
		iowrite32(0xffffffff, bank_reg(gpio, bank, reg_irq_type0));
		/* trigger type is level */
		iowrite32(0xffffffff, bank_reg(gpio, bank, reg_irq_type1));
		/* single trigger mode */
		iowrite32(0x00000000, bank_reg(gpio, bank, reg_irq_type2));
	}

	rc = platform_get_irq(pdev, 0);
	if (rc < 0)
		return rc;

	gpio->irq = rc;
	girq = &gpio->chip.irq;
	girq->chip = &gpio->irqc;
	girq->chip->name = dev_name(&pdev->dev);
	girq->chip->irq_ack = aspeed_sgpio_irq_ack;
	girq->chip->irq_mask = aspeed_sgpio_irq_mask;
	girq->chip->irq_unmask = aspeed_sgpio_irq_unmask;
	girq->chip->irq_set_type = aspeed_sgpio_set_type;
	girq->parent_handler = aspeed_sgpio_irq_handler;
	girq->num_parents = 1;
	girq->parents = devm_kcalloc(&pdev->dev, 1,
				     sizeof(*girq->parents),
				     GFP_KERNEL);
	if (!girq->parents)
		return -ENOMEM;
	girq->parents[0] = gpio->irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_bad_irq;
	girq->init_valid_mask = aspeed_sgpio_init_irq_valid_mask;

	return 0;
}

static int aspeed_sgpio_reset_tolerance(struct gpio_chip *chip,
					unsigned int offset, bool enable)
{
	struct aspeed_sgpio *gpio = gpiochip_get_data(chip);
	unsigned long flags;
	void __iomem *treg;
	u32 val;

	treg = bank_reg(gpio, to_bank(offset), reg_tolerance);

	spin_lock_irqsave(&gpio->lock, flags);

	val = readl(treg);

	if (enable)
		val |= GPIO_BIT(offset);
	else
		val &= ~GPIO_BIT(offset);

	writel(val, treg);

	spin_unlock_irqrestore(&gpio->lock, flags);

	return 0;
}

static int aspeed_sgpio_set_config(struct gpio_chip *chip, unsigned int offset,
				  unsigned long config)
{
	unsigned long param = pinconf_to_config_param(config);
	u32 arg = pinconf_to_config_argument(config);

	if (param == PIN_CONFIG_PERSIST_STATE)
		return aspeed_sgpio_reset_tolerance(chip, offset, arg);

	return -ENOTSUPP;
}

/*
 * Any banks not specified in a struct aspeed_bank_props array are assumed to
 * have the properties:
 *
 *     { .input = 0xffffffff, .output = 0xffffffff }
 */

static const struct aspeed_bank_props ast_sgpio_bank_props[] = {
	/*     input	  output   */
	{ 0, 0x00000000, 0xffffffff }, /* OA/OB/OC/OD */
	{ 1, 0x00000000, 0xffffffff }, /* OE/OF/OG/OH */
	{ 2, 0x00000000, 0x0000ffff }, /* OI/OJ */
	{ 3, 0xffffffff, 0x00000000 }, /* IA/IB/IC/ID */
	{ 4, 0xffffffff, 0x00000000 }, /* IE/IF/IG/IH */
	{ 5, 0x0000ffff, 0x00000000 }, /* II/IJ */
	{ }
};

/*
 * This H/W has 80 bidirectional lines so this driver provides total 160 lines
 * for 80 outputs and 80 inputs. To simplify bank register manipulation, it
 * uses 96 lines per each input and output set so total 192 lines it has.
 */
static const struct aspeed_sgpio_config ast2400_config =
	{ .nr_pgpios = 224, .nr_gpios = 192, .props = ast_sgpio_bank_props };

static const struct aspeed_sgpio_config ast2500_config =
	{ .nr_pgpios = 232, .nr_gpios = 192, .props = ast_sgpio_bank_props };

static const struct of_device_id aspeed_sgpio_of_table[] = {
	{ .compatible = "aspeed,ast2400-sgpio", .data = &ast2400_config },
	{ .compatible = "aspeed,ast2500-sgpio", .data = &ast2500_config },
	{ .compatible = "aspeed,ast2600-sgpio", .data = &ast2500_config },
	{ }
};
MODULE_DEVICE_TABLE(of, aspeed_sgpio_of_table);

static int __init aspeed_sgpio_probe(struct platform_device *pdev)
{
	const struct of_device_id *gpio_id;
	u32 sgpio_freq, clk_div, nb_gpios;
	struct aspeed_sgpio *gpio;
	unsigned long src_freq;
	struct clk *clk;
	int rc;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->base))
		return PTR_ERR(gpio->base);

	spin_lock_init(&gpio->lock);

	gpio_id = of_match_node(aspeed_sgpio_of_table, pdev->dev.of_node);
	if (!gpio_id)
		return -EINVAL;

	gpio->config = gpio_id->data;

	rc = device_property_read_u32(&pdev->dev, "bus-frequency", &sgpio_freq);
	if (rc < 0) {
		dev_warn(&pdev->dev, "Could not read bus-frequency property. Use default.\n");
		sgpio_freq = ASPEED_SGPIO_BUS_FREQ_DEFAULT;
	}

	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		rc = PTR_ERR(clk);
		if (rc != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to get clk source.\n");
		return rc;
	}

	/*
	 * There is a limitation that SGPIO clock division has to be larger or
	 * equal to 1. And a read back value of clock division is 1-bit left
	 * shifted from the actual value.
	 *
	 * GPIO254[31:16] - Serial GPIO clock division:
	 *  Serial GPIO clock period = period of PCLK * 2 * (GPIO254[31:16] + 1)
	 *
	 * SGPIO master controller updates every data input when SGPMLD is low.
	 * For an example, SGPIO clock is 1MHz and number of SGPIO is 80. Each
	 * SGPIO will be updated every 80us.
	 */
	src_freq = clk_get_rate(clk);
	clk_div = src_freq / (2 * sgpio_freq) - 1;
	if (clk_div < ASPEED_SGPIO_CLK_DIV_MIN)
		clk_div = ASPEED_SGPIO_CLK_DIV_MIN;
	else if (clk_div > ASPEED_SGPIO_CLK_DIV_MAX)
		clk_div = ASPEED_SGPIO_CLK_DIV_MAX;

	nb_gpios = gpio->config->nr_gpios / 16;
	if (nb_gpios < ASPEED_SGPIO_PINBYTES_MIN)
		nb_gpios = ASPEED_SGPIO_PINBYTES_MIN;
	else if (nb_gpios > ASPEED_SGPIO_PINBYTES_MAX)
		nb_gpios = ASPEED_SGPIO_PINBYTES_MAX;

	iowrite32(FIELD_PREP(ASPEED_SGPIO_CLK_DIV_MASK, clk_div) |
		  FIELD_PREP(ASPEED_SGPIO_PINBYTES_MASK, nb_gpios) |
		  ASPEED_SGPIO_ENABLE,
		  gpio->base + ASPEED_SGPIO_CTRL);

	gpio->chip.parent = &pdev->dev;
	gpio->chip.ngpio = gpio->config->nr_gpios;

	gpio->chip.direction_input = aspeed_sgpio_dir_in;
	gpio->chip.direction_output = aspeed_sgpio_dir_out;
	gpio->chip.get_direction = aspeed_sgpio_get_direction;
	gpio->chip.get = aspeed_sgpio_get;
	gpio->chip.set = aspeed_sgpio_set;
	gpio->chip.set_config = aspeed_sgpio_set_config;
	gpio->chip.label = dev_name(&pdev->dev);
	gpio->chip.base = gpio->config->nr_pgpios;

	rc = aspeed_sgpio_setup_irqs(gpio, pdev);
	if (rc < 0)
		return rc;

	return devm_gpiochip_add_data(&pdev->dev, &gpio->chip, gpio);
}

static struct platform_driver aspeed_sgpio_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = aspeed_sgpio_of_table,
	},
};

module_platform_driver_probe(aspeed_sgpio_driver, aspeed_sgpio_probe);

MODULE_AUTHOR("Jae Hyun Yoo <jae.hyun.yoo@linux.intel.com>");
MODULE_DESCRIPTION("Aspeed SGPIO Master Driver");
MODULE_LICENSE("GPL v2");
