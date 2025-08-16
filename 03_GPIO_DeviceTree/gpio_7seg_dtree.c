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
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#define DEV_NAME "gpio_7seg_dts"
#define NUM_SEGMENTS 7
#define DISPLAYS 2

static struct gpio_desc *segments[NUM_SEGMENTS] = {NULL};
static struct gpio_desc *cathodes[DISPLAYS] = {NULL};

static char display_buffer[DISPLAYS] = {'0', '0'};
static int current_digit = 0;

static const bool digit_map[10][NUM_SEGMENTS] = {
    {1, 1, 1, 1, 1, 1, 0}, // 0
    {0, 1, 1, 0, 0, 0, 0}, // 1
    {1, 1, 0, 1, 1, 0, 1}, // 2
    {1, 1, 1, 1, 0, 0, 1}, // 3
    {0, 1, 1, 0, 0, 1, 1}, // 4
    {1, 0, 1, 1, 0, 1, 1}, // 5
    {1, 0, 1, 1, 1, 1, 1}, // 6
    {1, 1, 1, 0, 0, 0, 0}, // 7
    {1, 1, 1, 1, 1, 1, 1}, // 8
    {1, 1, 1, 1, 0, 1, 1}, // 9
};

static int setup_gpios(struct device *dev)
{
    int i, j;

    for (i = 0; i < NUM_SEGMENTS; i++)
    {
        segments[i] = devm_gpiod_get_index(dev, "segment", i, GPIOD_OUT_LOW);
        if (IS_ERR(segments[i]))
        {
            dev_err(dev, "Failed to get segment %d GPIO\n", i);
            return PTR_ERR(segments[i]);
        }
        dev_info(dev, "segment[%d] -> GPIO %d\n", i, desc_to_gpio(segments[i]));
    }

    for (j = 0; j < DISPLAYS; j++)
    {
        cathodes[j] = devm_gpiod_get_index(dev, "cathode", j, GPIOD_OUT_HIGH);
        if (IS_ERR(cathodes[j]))
        {
            dev_err(dev, "Failed to get cathode %d GPIO\n", j);
            return PTR_ERR(cathodes[j]);
        }
    }

    dev_info(dev, "All GPIOs configured successfully\n");
    return 0;
}

static void display_digit_at(int idx)
{
    int digit, i;
    char c = display_buffer[idx];

    /* Turn both digits off before switching */
    gpiod_set_value(cathodes[0], 0);
    gpiod_set_value(cathodes[1], 0);

    if (c < '0' || c > '9')
        return;

    digit = c - '0';
    for (i = 0; i < NUM_SEGMENTS; i++)
        gpiod_set_value(segments[i], digit_map[digit][i]);

    /* Enable only the selected digit */
    gpiod_set_value(cathodes[idx], 1);
}

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

static ssize_t dev_write(struct file *file, const char __user *buf, size_t count, loff_t *f_pos)
{
    char kbuf[128];

    if (count == 0)
        return 0;

    if (count > sizeof(kbuf) - 1)
        return -EINVAL;

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';
    pr_info("%s: Received: %s\n", DEV_NAME, kbuf);

    if (count >= 2)
    {
        display_buffer[0] = kbuf[0];
        display_buffer[1] = kbuf[1];
    }
    else if (count == 1)
    {
        display_buffer[0] = kbuf[0];
        display_buffer[1] = ' '; /* blank second digit */
    }

    pr_info("%s: will display: %c%c\n", DEV_NAME, display_buffer[0], display_buffer[1]);
    return count;
}

static const struct file_operations dev_fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .write = dev_write,
};

/* Use misc device to let kernel/udev create /dev entry automatically */
static struct miscdevice sevenseg_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEV_NAME,
    .mode = 0666,
    .fops = &dev_fops,
};

/* ========== Timer / strobe ========== */

#define STROBE_MS 10
static struct hrtimer strobe_timer;
static ktime_t strobe_interval;

static enum hrtimer_restart strobe_func(struct hrtimer *t)
{
    display_digit_at(current_digit);
    current_digit ^= 1;
    hrtimer_forward_now(t, strobe_interval);
    return HRTIMER_RESTART;
}

static void start_strobe(void)
{
    strobe_interval = ktime_set(0, STROBE_MS * 1e6);
    hrtimer_init(&strobe_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    strobe_timer.function = strobe_func;
    hrtimer_start(&strobe_timer, strobe_interval, HRTIMER_MODE_REL);
}

static void stop_strobe(void)
{
    hrtimer_cancel(&strobe_timer);
}

/* ======== Platform driver integration ======== */

static int sevenseg_probe(struct platform_device *pdev)
{
    int ret;

    ret = setup_gpios(&pdev->dev);
    if (ret)
        return ret;

    ret = misc_register(&sevenseg_misc);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register misc device: %d\n", ret);
        return ret;
    }

    start_strobe();
    dev_info(&pdev->dev, "%s: initialized (minor %d)\n", DEV_NAME, sevenseg_misc.minor);
    return 0;
}

static void sevenseg_remove(struct platform_device *pdev)
{
    stop_strobe();
    misc_deregister(&sevenseg_misc);
    dev_info(&pdev->dev, "%s: removed\n", DEV_NAME);
    return;
}

static const struct of_device_id sevenseg_of_match[] = {
    {.compatible = "arg,sevenseg"},
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, sevenseg_of_match);

static struct platform_driver sevenseg_driver = {
    .driver = {
        .name = "sevenseg",
        .of_match_table = sevenseg_of_match,
    },
    .probe = sevenseg_probe,
    .remove = sevenseg_remove,
};

module_platform_driver(sevenseg_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("7-Segment Display Driver (Common Cathode, Device Tree)");
