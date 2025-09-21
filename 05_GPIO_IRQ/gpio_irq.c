#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/miscdevice.h>

#define DEV_NAME "gpio_irq"

static __u32 irq_counter = 0;

/* IRQ/debounce state */
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>

#define DEBOUNCE_MS 200
static spinlock_t irq_lock;
static unsigned long last_jiffies = 0;

/* ========== Character device (misc) ========== */

static int dev_open(struct inode *inode, struct file *file)
{
    pr_info("%s: Device opened\n", DEV_NAME);
    return 0;
}

static int dev_release(struct inode *inode, struct file *file)
{
    pr_info("%s: Device closed\n", DEV_NAME);
    return 0;
}

static ssize_t dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[64];
    int len;
    u32 local_count = irq_counter; /* snapshot to avoid races */

    len = snprintf(kbuf, sizeof(kbuf), "IRQ count: %u\n", local_count);
    if (len < 0)
        return len;

    return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t)len);
}

static const struct file_operations dev_fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .read = dev_read,
};

/* Use misc device to let kernel/udev create /dev entry automatically */
static struct miscdevice miscdevice_struct = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEV_NAME,
    .mode = 0666,
    .fops = &dev_fops,
};

/* ========== IRQ handling with simple debounce ========== */

/*
 * IRQ handler: perform very short critical check using jiffies-based
 * debounce. We increment the counter only when the last event was older than
 * DEBOUNCE_MS. Access to last_jiffies and irq_counter is protected by a spinlock.
 *
 * Jiffies is a global counter that increments at HZ frequency (typically 100 or 250 Hz).
 * We use time_after() to handle jiffies wrap-around correctly.
 *
 * A spinlock is: a lightweight lock that can be used in interrupt context.
 */
static irqreturn_t gpio_irq_handler(int irq, void *dev_id)
{
    unsigned long flags;
    unsigned long now = jiffies;

    spin_lock_irqsave(&irq_lock, flags);
    if (!time_after(now, last_jiffies + msecs_to_jiffies(DEBOUNCE_MS)))
    {
        /* within debounce window -> ignore */
        spin_unlock_irqrestore(&irq_lock, flags);
        return IRQ_HANDLED;
    }

    last_jiffies = now;
    irq_counter++;
    spin_unlock_irqrestore(&irq_lock, flags);

    pr_info("%s: irq fired, count=%u\n", DEV_NAME, irq_counter);
    return IRQ_HANDLED;
}

/* ======== Platform driver integration ======== */

static int platform_device_probe(struct platform_device *pdev)
{
    int ret, irq;

    /* init lock used by IRQ handler */
    spin_lock_init(&irq_lock);

    /* obtain IRQ from DT (interrupts property) */
    irq = platform_get_irq(pdev, 0);
    if (irq < 0)
    {
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

    ret = misc_register(&miscdevice_struct);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register misc device: %d\n", ret);
        return ret;
    }

    dev_info(&pdev->dev, "%s: initialized (minor %d)\n", DEV_NAME, miscdevice_struct.minor);
    return 0;
}

static void platform_device_remove(struct platform_device *pdev)
{
    misc_deregister(&miscdevice_struct);
    dev_info(&pdev->dev, "%s: removed\n", DEV_NAME);
    return;
}

static const struct of_device_id device_of_match[] = {
    {.compatible = "arg,irq-example"},
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, device_of_match);

static struct platform_driver platform_driver_struct = {
    .driver = {
        .name = "irq-example",
        .of_match_table = device_of_match,
    },
    .probe = platform_device_probe,
    .remove = platform_device_remove,
};

module_platform_driver(platform_driver_struct);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("IRQ example with GPIO and misc device");
