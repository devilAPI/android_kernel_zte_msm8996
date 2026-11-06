/*
 * AH1898 hall sensor (flip cover detection)
 *
 * Ported from the ZTE Axon 7 3.18 kernel (drivers/input/misc/ah1898.c,
 * author chenhui). The sensor output is a GPIO that is low while a magnet
 * (cover) is near. It is reported as the SW_LID input switch.
 *
 * Changes from the 3.18 driver: the ZTE-only KEY_HALL_SENSOR_UP/DOWN key
 * events and the hook into ZTE's Synaptics driver are dropped, the IRQ is
 * handled in a threaded handler instead of a work item, resources are
 * device managed, and the initial state is reported at probe.
 *
 * Licensed under the GPL, like the original driver (MODULE_LICENSE "GPL").
 */

#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

/* 1 = closed (magnet near), 0 = open; read-only mirror of the switch state */
static int hall_status;
module_param(hall_status, int, 0444);
MODULE_PARM_DESC(hall_status, "Current hall state (1 = cover closed)");

struct ah1898_chip {
	struct input_dev *input;
	int gpio;
	int irq;
};

static void ah1898_report(struct ah1898_chip *chip)
{
	/* the output is pulled low while the magnet is near */
	hall_status = gpio_get_value(chip->gpio) ? 0 : 1;

	input_report_switch(chip->input, SW_LID, hall_status);
	input_sync(chip->input);
}

static irqreturn_t ah1898_irq_thread(int irq, void *data)
{
	struct ah1898_chip *chip = data;

	ah1898_report(chip);
	dev_dbg(chip->input->dev.parent, "cover %s\n",
		hall_status ? "closed" : "open");

	return IRQ_HANDLED;
}

static int ah1898_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ah1898_chip *chip;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->gpio = of_get_named_gpio(dev->of_node, "ah,gpio_irq", 0);
	if (!gpio_is_valid(chip->gpio)) {
		dev_err(dev, "invalid ah,gpio_irq %d\n", chip->gpio);
		return chip->gpio == -EPROBE_DEFER ? -EPROBE_DEFER : -EINVAL;
	}

	ret = devm_gpio_request_one(dev, chip->gpio, GPIOF_IN, "ah1898_irq");
	if (ret) {
		dev_err(dev, "failed to request gpio %d: %d\n", chip->gpio, ret);
		return ret;
	}

	chip->irq = gpio_to_irq(chip->gpio);
	if (chip->irq < 0) {
		dev_err(dev, "no irq for gpio %d: %d\n", chip->gpio, chip->irq);
		return chip->irq;
	}

	chip->input = devm_input_allocate_device(dev);
	if (!chip->input)
		return -ENOMEM;

	chip->input->name = "ah1898";
	chip->input->phys = "ah1898/input0";
	chip->input->id.bustype = BUS_HOST;
	input_set_capability(chip->input, EV_SW, SW_LID);

	ret = input_register_device(chip->input);
	if (ret) {
		dev_err(dev, "failed to register input device: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, chip);

	/* report the state at boot so the switch bitmap is valid */
	ah1898_report(chip);

	ret = devm_request_threaded_irq(dev, chip->irq, NULL,
					ah1898_irq_thread,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"ah1898_irq", chip);
	if (ret) {
		dev_err(dev, "failed to request irq %d: %d\n", chip->irq, ret);
		return ret;
	}

	device_init_wakeup(dev, true);

	dev_info(dev, "hall sensor on gpio %d, cover %s\n", chip->gpio,
		 hall_status ? "closed" : "open");

	return 0;
}

static int ah1898_remove(struct platform_device *pdev)
{
	device_init_wakeup(&pdev->dev, false);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int ah1898_suspend(struct device *dev)
{
	struct ah1898_chip *chip = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(chip->irq);

	return 0;
}

static int ah1898_resume(struct device *dev)
{
	struct ah1898_chip *chip = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(chip->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(ah1898_pm_ops, ah1898_suspend, ah1898_resume);

static const struct of_device_id ah1898_of_match[] = {
	{ .compatible = "ah,hall_ic", },
	{ },
};
MODULE_DEVICE_TABLE(of, ah1898_of_match);

static struct platform_driver ah1898_driver = {
	.probe = ah1898_probe,
	.remove = ah1898_remove,
	.driver = {
		.name = "hall_ic",
		.of_match_table = ah1898_of_match,
		.pm = &ah1898_pm_ops,
	},
};
module_platform_driver(ah1898_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("chenhui");
MODULE_DESCRIPTION("AH1898 hall sensor driver");
MODULE_ALIAS("platform:hall_ic");
