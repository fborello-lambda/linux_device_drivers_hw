#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>

#define DEV_NAME "gpio_7seg"
#define IO_OFFSET 512
#define NUM_SEGMENTS 7

static struct gpio_desc *segments[7] = {NULL};
static struct gpio_desc *cathode_enable = NULL;

#define A 24
#define B 23
#define C 20
#define D 21
#define E 22
#define F 25
#define G 19

static const int gpio_nums[NUM_SEGMENTS] = {A, B, C, D, E, F, G};
static const int cathode_gpio_num = 17;

static const bool digit_map[10][7] = {
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

static int setup_gpios(void)
{
    int i, ret = 0;

    for (i = 0; i < NUM_SEGMENTS; i++)
    {
        segments[i] = gpio_to_desc(gpio_nums[i] + IO_OFFSET);
        if (IS_ERR(segments[i]))
        {
            pr_err("Failed to get gpio_desc for segment %d (GPIO %d)\n", i, gpio_nums[i]);
            ret = PTR_ERR(segments[i]);
            goto fail;
        }
        ret = gpiod_direction_output(segments[i], 0);
        if (ret)
        {
            pr_err("Failed to set direction output for segment %d\n", i);
            goto fail;
        }
    }

    cathode_enable = gpio_to_desc(cathode_gpio_num + IO_OFFSET);
    if (IS_ERR(cathode_enable))
    {
        pr_err("Failed to get gpio_desc for cathode GPIO %d\n", cathode_gpio_num);
        ret = PTR_ERR(cathode_enable);
        goto fail;
    }
    // Como cátodo activo en 0, seteamos 1 para apagar al inicio
    ret = gpiod_direction_output(cathode_enable, 1);
    if (ret)
    {
        pr_err("Failed to set direction output for cathode\n");
        goto fail;
    }

    return 0;

fail:
    while (--i >= 0)
        gpiod_put(segments[i]);
    if (!IS_ERR_OR_NULL(cathode_enable))
        gpiod_put(cathode_enable);
    return ret;
}

static void release_gpios(void)
{
    int i;

    if (!IS_ERR_OR_NULL(cathode_enable))
    {
        gpiod_put(cathode_enable);
        cathode_enable = NULL;
    }

    for (i = 0; i < NUM_SEGMENTS; i++)
    {
        if (!IS_ERR_OR_NULL(segments[i]))
        {
            gpiod_set_value(segments[i], 0);
            gpiod_put(segments[i]);
            segments[i] = NULL;
        }
    }
}
static void display_digit(char c)
{
    int digit, i;

    if (c < '0' || c > '9')
    {
        gpiod_set_value(cathode_enable, 1);
        return;
    }

    digit = c - '0';

    for (i = 0; i < NUM_SEGMENTS; i++)
        gpiod_set_value(segments[i], digit_map[digit][i]);

    pr_info("%s: set to: %i\n", DEV_NAME, digit);

    gpiod_set_value(cathode_enable, 0);
}

// ========== Device logic ==========

static int dev_major = 0;
static int dev_minor = 0;
static dev_t dev_number;
static struct cdev dev_cdev;

module_param(dev_major, int, S_IRUGO);
module_param(dev_minor, int, S_IRUGO);

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

    if (count > sizeof(kbuf) - 1)
        return -EINVAL;

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';

    pr_info("%s: Received: %s\n", DEV_NAME, kbuf);

    if (count > 0)
    {
        pr_info("%s: is going to display: %c\n", DEV_NAME, (char)kbuf[0]);
        display_digit((char)kbuf[0]);
    }
    return count;
}

static struct file_operations dev_fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .write = dev_write,
};

// ========== Init/Exit ==========

static int __init dev_init(void)
{
    int result;

    result = setup_gpios();
    if (result)
    {
        pr_warn("%s: GPIO setup failed\n", DEV_NAME);
        return result;
    }

    if (dev_major)
    {
        dev_number = MKDEV(dev_major, dev_minor);
        result = register_chrdev_region(dev_number, 1, DEV_NAME);
    }
    else
    {
        result = alloc_chrdev_region(&dev_number, dev_minor, 1, DEV_NAME);
        dev_major = MAJOR(dev_number);
    }
    if (result < 0)
    {
        pr_warn("%s: can't get major %d\n", DEV_NAME, dev_major);
        goto err_gpios;
    }

    cdev_init(&dev_cdev, &dev_fops);
    dev_cdev.owner = THIS_MODULE;

    result = cdev_add(&dev_cdev, dev_number, 1);
    if (result)
    {
        goto err_chrdev;
    }

    pr_info("%s: Initialized (major %d)\n", DEV_NAME, dev_major);
    return 0;

err_chrdev:
    unregister_chrdev_region(dev_number, 1);
err_gpios:
    release_gpios();
    return result;
}

static void __exit dev_exit(void)
{
    release_gpios();
    cdev_del(&dev_cdev);
    unregister_chrdev_region(dev_number, 1);
    pr_info("%s: Exiting\n", DEV_NAME);
}

module_init(dev_init);
module_exit(dev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("7-Segment Display Driver (Common Cathode)");
