#include <linux/module.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
/* MPU6050 primitives */
#include "mpu6050_kdd_primitives.h"
/* I2C primitives */
#include "i2c2_ll.h"

// Do not change.
#define DEV_NAME "mpu6050_pdev"

/* Simple state for last read sample (optional) */
static struct
{
    spinlock_t lock;
    bool valid;
    mpu6050_raw_t raw;
    mpu6050_sample_fixed_t fixed;
} g_sample_state = {
    .lock = __SPIN_LOCK_UNLOCKED(g_sample_state.lock),
    .valid = false,
};

// Global device state
static mpu6050_t g_mpu6050_dev = {
    .i2c_addr = MPU6050_I2C_ADDR_DEFAULT,
    .initialized = 0,
};

static const struct of_device_id mpu_of_match[] = {
    {.compatible = "arg,i2c2-ll"},
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, mpu_of_match);

static int init_mpu6050(void)
{
    __u8 ret;
    /* Read the chip ID */
    if (mpu6050_kdd_whoami(&g_mpu6050_dev, &ret) != MPU6050_OK)
    {
        pr_err("%s: Failed to read WHOAMI register\n", DEV_NAME);
        return -EIO;
    }
    if (ret != MPU6050_I2C_ADDR_DEFAULT)
    { // expected MPU6050 address
        pr_err("%s: Unexpected WHOAMI: 0x%02x\n", DEV_NAME, ret);
        return -ENODEV;
    }
    pr_info("%s: Detected WHOAMI: 0x%02x\n", DEV_NAME, ret);

    if (mpu6050_kdd_init(&g_mpu6050_dev, (mpu6050_config_full_t)DEFAULT_MPU6050_CONFIG) != MPU6050_OK)
    {
        pr_err("%s: Failed to initialize device registers\n", DEV_NAME);
        return -EIO;
    }

    return 0;
}

static int remove_mpu6050(void)
{
    if (mpu6050_kdd_reset(&g_mpu6050_dev) != MPU6050_OK)
    {
        pr_err("%s: Failed to reset\n", DEV_NAME);
        return -EIO;
    }

    return 0;
}

/* ========== Character device (misc) ========== */
static ssize_t dev_read(struct file *file, char __user *buf,
                        size_t count, loff_t *ppos)
{
    char kbuf[256];
    int pos = 0;
    mpu6050_raw_t r;
    mpu6050_sample_fixed_t fx;
    bool valid;
    unsigned long flags;

    /* Capture MPU6050 sample state */
    spin_lock_irqsave(&g_sample_state.lock, flags);
    valid = g_sample_state.valid;
    if (valid)
    {
        r = g_sample_state.raw; /* struct copy */
        fx = g_sample_state.fixed;
    }
    spin_unlock_irqrestore(&g_sample_state.lock, flags);

    if (!valid)
    {
        /* On-demand read if no cached sample */
        __u8 who;
        if (mpu6050_kdd_whoami(&g_mpu6050_dev, &who) == MPU6050_OK)
        {
            pos += scnprintf(kbuf + pos, sizeof(kbuf) - pos,
                             "WHOAMI: 0x%02x\n(no cached sample)\n", who);
            pr_info("%s: Detected WHOAMI: 0x%02x\n", DEV_NAME, who);
        }
        else
        {
            pos += scnprintf(kbuf + pos, sizeof(kbuf) - pos,
                             "WHOAMI read failed\n");
        }
    }
    else
    {
        pos += mpu6050_kdd_print_msg(kbuf + pos, sizeof(kbuf) - pos, &r, &fx, 0, 1);
    }

    return simple_read_from_buffer(buf, count, ppos, kbuf, pos);
}

static const struct file_operations dev_fops = {
    .owner = THIS_MODULE,
    .read = dev_read,
};

/* Set device node permissions when created under /dev */
static char *mpu_class_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666; /* world-readable and writable */
    return NULL;      /* keep default node name */
}

/* Platform/char device state stored per platform device */
struct pdev_char_data
{
    dev_t devno;
    struct cdev cdev;
    struct class *class;
    struct device *device;
};

/* ======== Platform driver to create char device ======== */
static int mpu_platform_probe(struct platform_device *pdev)
{
    struct pdev_char_data *pdata;
    int ret;
    u32 bus_freq = 100000; /* Hz */
    u32 bus_khz;
    struct device_node *child;
    int irq;

    // POSIBILITY: The register map could be provided by DT.
    if (pdev->dev.of_node)
    {
        if (!of_property_read_u32(pdev->dev.of_node, "bus-frequency", &bus_freq) &&
            bus_freq == 0)
            bus_freq = 100000;
    }

    bus_khz = max(1U, bus_freq / 1000);

    /* Get I2C2 IRQ from DT and initialize low-level I2C with it */
    irq = platform_get_irq(pdev, 0);
    if (irq < 0)
    {
        dev_err(&pdev->dev, "failed to get I2C2 IRQ from DT: %d\n", irq);
        return irq;
    }

    ret = i2c2_ll_init(bus_khz, irq);
    if (ret)
    {
        dev_err(&pdev->dev, "i2c2_ll_init failed: %d\n", ret);
        return ret;
    }

    /* Adopt the I2C address from the first compatible child, if provided */
    if (pdev->dev.of_node)
    {
        for_each_child_of_node(pdev->dev.of_node, child)
        {
            if (of_device_is_compatible(child, "arg,kdr_mpu6050"))
            {
                u32 addr;
                if (!of_property_read_u32(child, "reg", &addr))
                    g_mpu6050_dev.i2c_addr = addr & 0x7f;
                break;
            }
        }
    }

    ret = init_mpu6050();
    if (ret)
    {
        dev_err(&pdev->dev, "init_mpu6050 failed: %d\n", ret);
        i2c2_ll_deinit();
        return ret;
    }

    /* Only after successful init, create the char device */
    pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
    if (!pdata)
    {
        i2c2_ll_deinit();
        return -ENOMEM;
    }

    /* Allocate device number */
    ret = alloc_chrdev_region(&pdata->devno, 0, 1, DEV_NAME);
    if (ret)
    {
        dev_err(&pdev->dev, "alloc_chrdev_region failed: %d\n", ret);
        i2c2_ll_deinit();
        return ret;
    }

    cdev_init(&pdata->cdev, &dev_fops);
    pdata->cdev.owner = THIS_MODULE;
    ret = cdev_add(&pdata->cdev, pdata->devno, 1);
    if (ret)
    {
        dev_err(&pdev->dev, "cdev_add failed: %d\n", ret);
        goto err_unregister_chrdev;
    }

    /* Note: On this kernel, class_create(name) takes only the class name */
    pdata->class = class_create(DEV_NAME);
    if (IS_ERR(pdata->class))
    {
        ret = PTR_ERR(pdata->class);
        dev_err(&pdev->dev, "class_create failed: %d\n", ret);
        goto err_cdev_del;
    }

    /* Set device node mode so /dev/mpu6050_pdev is accessible by all users */
    pdata->class->devnode = mpu_class_devnode;

    pdata->device = device_create(pdata->class, &pdev->dev, pdata->devno, NULL, DEV_NAME);
    if (IS_ERR(pdata->device))
    {
        ret = PTR_ERR(pdata->device);
        dev_err(&pdev->dev, "device_create failed: %d\n", ret);
        goto err_class_destroy;
    }

    platform_set_drvdata(pdev, pdata);
    dev_info(&pdev->dev, "char device created (major=%d, minor=%d)\n", MAJOR(pdata->devno), MINOR(pdata->devno));
    return 0;

err_class_destroy:
    class_destroy(pdata->class);
err_cdev_del:
    cdev_del(&pdata->cdev);
err_unregister_chrdev:
    unregister_chrdev_region(pdata->devno, 1);
    i2c2_ll_deinit();
    return ret;
}

static void mpu_platform_remove(struct platform_device *pdev)
{
    struct pdev_char_data *pdata = platform_get_drvdata(pdev);
    if (!pdata)
        return;

    dev_info(&pdev->dev, "char device removed\n");
    remove_mpu6050();
    i2c2_ll_deinit();
}
/* Synthetic platform device support (for non-DT boots) */
static struct platform_device *g_pdev;

static struct platform_driver mpu_platform_driver = {
    .probe = mpu_platform_probe,
    .remove = mpu_platform_remove,
    .driver = {
        .name = DEV_NAME,
        .of_match_table = mpu_of_match,
    },
};

static int __init mpu_module_init(void)
{
    int ret;
    struct device_node *np;

    ret = platform_driver_register(&mpu_platform_driver);
    if (ret)
        return ret;

    /* Only create the synthetic device if no DT node references us */
    np = of_find_matching_node(NULL, mpu_of_match);
    if (!np)
    {
        g_pdev = platform_device_register_simple(DEV_NAME, -1, NULL, 0);
        if (IS_ERR(g_pdev))
        {
            ret = PTR_ERR(g_pdev);
            g_pdev = NULL;
            pr_err("Failed to register platform device: %d\n", ret);
            platform_driver_unregister(&mpu_platform_driver);
            return ret;
        }
    }
    else
    {
        of_node_put(np);
        pr_info("mpu driver: DT node detected, skipping synthetic platform device\n");
    }

    pr_info("mpu driver initialized\n");
    return 0;
}

static void __exit mpu_module_exit(void)
{
    if (g_pdev)
    {
        platform_device_unregister(g_pdev);
        g_pdev = NULL;
    }
    platform_driver_unregister(&mpu_platform_driver);
    pr_info("mpu driver exited\n");
}

module_init(mpu_module_init);
module_exit(mpu_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("MPU6050 with I2C2 low-level driver, FIFO buffer enabled and checking IRQ status");
