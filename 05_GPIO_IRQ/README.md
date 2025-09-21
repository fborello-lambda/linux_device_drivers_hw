<h1 align="center">
    GPIO IRQ
</h1>

### What

This example shows how to create a simple GPIO driver that uses interrupts to handle button presses. The driver is implemented as a **platform driver** using a **device tree overlay** to declare the hardware resources (GPIOs).

There are two ways to link a gpio to an Interrupt Handler:

#### 1. Using the **gpiod_to_irq()** function to map a GPIO descriptor to an IRQ number

Inside the `probe()` function of the platform driver, the GPIO is requested and configured as an input. Then, `gpiod_to_irq()` is called to get the corresponding IRQ number. Finally, `devm_request_irq()` is used to register the interrupt handler for that IRQ. The gpio has to be configured, that's why the `setup_gpios()` function is called first.

```c
static int platform_device_probe(struct platform_device *pdev)
{
    int ret, irq;

    ret = setup_gpios(&pdev->dev);
    if (ret)
    return ret;

    /* ======== IRQ Setup Begin ======== */

    /* init lock used by IRQ handler */
    spin_lock_init(&irq_lock);

    /* convert gpiod to irq number */
    irq = gpiod_to_irq(irq_gpio);
    if (irq < 0)
    {
        dev_err(&pdev->dev, "Failed to map GPIO to IRQ: %d\n", irq);
        return irq;
    }

    /* request IRQ using falling edge trigger */
    ret = devm_request_irq(&pdev->dev, irq, gpio_irq_handler,
                           IRQF_TRIGGER_FALLING,
                           DEV_NAME, NULL);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", irq, ret);
        return ret;
    }
    /* ======== IRQ Setup Done ======== */

    ret = misc_register(&miscdevice_struct);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register misc device: %d\n", ret);
        return ret;
    }

    dev_info(&pdev->dev, "%s: initialized (minor %d)\n", DEV_NAME, miscdevice_struct.minor);
    return 0;
}
```

The `setup_gpios()` function uses `devm_gpiod_get()` to request the GPIO defined in the device tree. The `devm_` prefix indicates that the GPIO will be automatically released when the device is removed, simplifying resource management. It's a way of "parsing" the device tree entry.

```c
static int setup_gpios(struct device *dev)
{
    // matches irq-gpio in device tree
    irq_gpio = devm_gpiod_get_index(dev, "irq", 0, GPIOD_IN);
    if (IS_ERR(irq_gpio))
    {
        dev_err(dev, "Failed to get irq-gpio GPIO\n");
        return PTR_ERR(irq_gpio);
    }
    dev_info(dev, "irq-gpio -> GPIO %d\n", desc_to_gpio(irq_gpio));
    return 0;
}
```

> [!NOTE]
> The gpio is defined in the device tree overlay in the same way as in previous examples.

#### 2. Defining the GPIO as an interrupt in the device tree overlay

A device tree source file (`.dts`) is created to define the GPIOs used by the driver. Before being used it must be preprocessed (if it contains includes) and then compiled into a binary overlay file (`.dtbo`) using the device tree compiler (`dtc`). The usage of includes is optional, but it helps to keep the code cleaner and more maintainable. Maybe the number to configure the gpio as `IRQ_TYPE_EDGE_FALLING` is different on other boards, so it's better to use the symbolic name defined in the `dt-bindings` directory.

The `.dts` file:

```dts
/dts-v1/;
/plugin/;

#include <dt-bindings/interrupt-controller/irq.h>

/ {
    compatible = "brcm,bcm2835";

    fragment@0 {
        target-path = "/";
        __overlay__ {
            irq-example {
                compatible = "arg,irq-example";
                interrupt-parent = <&gpio>;
                interrupts = <23 IRQ_TYPE_EDGE_FALLING>;
            };
        };
    };
};
```

The `Makefile` is modified to include the preprocessing step (if needed) and to compile the device tree overlay. It also searches for the correct include path for the device tree bindings (`dt-bindings` directory) based on the current kernel version.

Then, in the `probe()` function of the platform driver, the IRQ number is obtained directly from the device tree using `platform_get_irq()`, and `devm_request_irq()` is used to register the interrupt handler for that IRQ. A thing to note is that the irq flags (like edge type) are set in the device tree, so no flags are needed when requesting the IRQ. Setting the flags to 0 means `IRQF_TRIGGER_NONE` (see [include/linux/irqflags.h](https://elixir.bootlin.com/linux/v6.16.8/source/include/linux/interrupt.h#L31)).

In this example, the irq is not threaded, so the interrupt handler must be quick and efficient. It simply increments a counter each time the button is pressed. A spinlock is used to protect access to the counter variable, ensuring that it is updated safely in the interrupt context. If more complex processing is needed `devm_request_threaded_irq()` can be used to create a threaded IRQ handler.

Finally, the `probe()` function just needs the following:

```c
//[...]
    /* obtain IRQ from DT (interrupts property) */
    irq = platform_get_irq(pdev, 0);
    if (irq < 0) {
        dev_err(&pdev->dev, "Failed to get IRQ from DT: %d\n", irq);
        return irq;
    }

    /* request IRQ — use 0 flags because trigger type is set by DT */
    ret = devm_request_irq(&pdev->dev, irq, gpio_irq_handler, 0, DEV_NAME, NULL);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", irq, ret);
        return ret;
    }
    dev_info(&pdev->dev, "%s: requested irq %d\n", DEV_NAME, irq);
//[...]
```

Note that the `setup_gpios()` function is not needed anymore, because the GPIO is configured as an interrupt in the device tree.

---

To make the example more robust, a debounce mechanism is implemented in the interrupt handler. This prevents multiple triggers from a single button press due to mechanical bouncing. The debounce time is defined by `DEBOUNCE_MS` (200 ms in this case). A spinlock is used to protect access to the `last_jiffies` variable, which stores the time of the last valid interrupt. If an interrupt occurs within the debounce period, it is ignored. So basically the debounce logic makes use of the `jiffies` variable that counts the number of ticks since the system started and a spinlock to ensure thread safety.

### How

There are some changes in the script, but the same commands apply:

```sh
./init_mod.sh mount
```

This compiles and loads the module, then creates a device file linked to the driver with `mknod`.

By connecting a button with a PULL-UP resistor to gpio pin 23 and pressing it, the interrupt handler is triggered, and the counter is incremented. The current count can be read from the device file:

```sh
cat /dev/gpio_irq
```

The output should be:

```txt
IRQ count: 3
```

To remove the module and delete the device file:

```sh
./init_mod.sh clean
```

## Resources

* [interrupt.h - include/linux/interrupt.h - Linux source code v6.16.8 - Bootlin Elixir Cross Referencer](https://elixir.bootlin.com/linux/v6.16.8/source/include/linux/interrupt.h#L215)
* [linux - Device tree compiler not recognizes C syntax for include files - Stack Overflow](https://stackoverflow.com/questions/50658326/device-tree-compiler-not-recognizes-c-syntax-for-include-files)
