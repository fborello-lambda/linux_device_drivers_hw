#include <linux/module.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/device.h>
/* IRQ headers */
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/of_irq.h>
/* I2C Headers */
#include <linux/uaccess.h>
#include <linux/i2c.h>
/* MPU6050 primitives */
#include "mpu6050_kdd_primitives.h"

// Do not change.
// Has to match the same name used in the .dts file.
#define DEV_NAME "mpu6050_pdev"

// IRQ/debounce state
#define DEBOUNCE_MS 200
static struct
{
    unsigned long debounce_ms;
    unsigned long last_jiffies;
    __u32 irq_counter;
    spinlock_t lock;

} g_irq_button_state = {
    .debounce_ms = DEBOUNCE_MS,
    .last_jiffies = 0,
    .irq_counter = 0,
    .lock = __SPIN_LOCK_UNLOCKED(g_irq_button_state.lock),
};

// MPU6050 sample state
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
    .client = NULL,
    .i2c_addr = MPU6050_I2C_ADDR_DEFAULT,
    .initialized = 0,
};

static int init_mpu6050(struct i2c_client *client)
{
    __u8 ret;
    g_mpu6050_dev.client = client;
    /* Read the chip ID */
    if (mpu6050_kdd_whoami(&g_mpu6050_dev, &ret) != MPU6050_OK)
        return dev_err_probe(&client->dev, ret, "Failed to read WHOAMI register\n");
    if (ret != MPU6050_I2C_ADDR_DEFAULT) // expected MPU6050 address
        return dev_err_probe(&client->dev, -ENODEV,
                             "Unexpected address: 0x%02x\n", ret);

    dev_info(&client->dev, "Detected with address: 0x%02x\n", ret);

    if (mpu6050_kdd_init(&g_mpu6050_dev, (mpu6050_config_full_t)DEFAULT_MPU6050_CONFIG, client) != MPU6050_OK)
    {
        return dev_err_probe(&client->dev, ret, "Failed to initialize\n");
    }

    return 0;
}

static int remove_mpu6050(struct i2c_client *client)
{
    if (mpu6050_kdd_reset(&g_mpu6050_dev) != MPU6050_OK)
    {
        dev_err(&client->dev, "Failed to reset\n");
        return -EIO;
    }

    return 0;
}

/* ========== MPU6050 IRQ ========== */
/* Thread (sleepable) IRQ handler */
static irqreturn_t mpu6050_irq_thread(int irq, void *dev_id)
{
    int st;

    if (!g_mpu6050_dev.client || !g_mpu6050_dev.initialized)
        return IRQ_NONE;

    st = i2c_smbus_read_byte_data(g_mpu6050_dev.client, MPU6050_REG_INT_STATUS);
    if (st < 0)
        return IRQ_HANDLED;

    if (st & MPU6050_INT_STATUS_FIFO_OFLOW)
    {
        mpu6050_kdd_reset_fifo(&g_mpu6050_dev);
        pr_warn("%s: FIFO overflow -> reset\n", DEV_NAME);
        return IRQ_HANDLED;
    }

    if (st & MPU6050_INT_STATUS_DATA_RDY)
    {
        mpu6050_raw_t fifo_sample;
        ssize_t n = mpu6050_read_fifo_samples(&g_mpu6050_dev, &fifo_sample, 1);
        if (n > 0)
        {
            mpu6050_sample_fixed_t fx;
            mpu6050_kdd_raw_to_sample_fixed(&g_mpu6050_dev, &fifo_sample, &fx);
            /* Publish sample */
            unsigned long flags;
            spin_lock_irqsave(&g_sample_state.lock, flags);
            g_sample_state.raw = fifo_sample; /* struct copy */
            g_sample_state.fixed = fx;
            g_sample_state.valid = true;
            spin_unlock_irqrestore(&g_sample_state.lock, flags);
        }
    }
    return IRQ_HANDLED;
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
    u32 local_irq_count;

    /* Capture IRQ count */
    spin_lock_irqsave(&g_irq_button_state.lock, flags);
    local_irq_count = g_irq_button_state.irq_counter;
    spin_unlock_irqrestore(&g_irq_button_state.lock, flags);

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
        pos = scnprintf(kbuf, sizeof(kbuf),
                        "IRQ count: %u\n(no sample yet)\n",
                        local_irq_count);
    }
    else
    {
        pos += scnprintf(kbuf + pos, sizeof(kbuf) - pos,
                         "IRQ count: %u\n", local_irq_count);

        pos += mpu6050_kdd_print_msg(kbuf + pos, sizeof(kbuf) - pos, &r, &fx, 0, 1);
    }

    return simple_read_from_buffer(buf, count, ppos, kbuf, pos);
}

static const struct file_operations dev_fops = {
    .owner = THIS_MODULE,
    .read = dev_read,
};

/* Optional: set device node permissions when created under /dev */
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
    struct i2c_client *client; /* pointer passed from i2c probe */
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

    spin_lock_irqsave(&g_irq_button_state.lock, flags);
    if (!time_after(now, g_irq_button_state.last_jiffies + msecs_to_jiffies(g_irq_button_state.debounce_ms)))
    {
        /* within debounce window -> ignore */
        spin_unlock_irqrestore(&g_irq_button_state.lock, flags);
        return IRQ_HANDLED;
    }

    g_irq_button_state.last_jiffies = now;
    g_irq_button_state.irq_counter++;
    pr_info("%s: irq fired, count=%u\n", DEV_NAME, g_irq_button_state.irq_counter);

    spin_unlock_irqrestore(&g_irq_button_state.lock, flags);
    return IRQ_HANDLED;
}

/* ======== I2C driver integration ======== */
static int i2c_device_probe(struct i2c_client *client)
{
    int irq0, irq1, ret;
    struct platform_device *pdev = NULL;

    /* obtain IRQ from Device Tree (interrupts property) */
    irq0 = of_irq_get(client->dev.of_node, 0);
    if (irq0 > 0)
    {
        ret = devm_request_threaded_irq(&client->dev,
                                        irq0,
                                        NULL,               /* no hard handler */
                                        mpu6050_irq_thread, /* threaded handler */
                                        IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
                                        DEV_NAME,
                                        client);
        if (ret)
        {
            dev_err(&client->dev, "Failed to request irq0 %d: %d\n", irq0, ret);
            return ret;
        }
    }
    dev_info(&client->dev, "Requested irq %d\n", irq0);

    irq1 = of_irq_get(client->dev.of_node, 1);
    if (irq1 > 0)
    {
        ret = devm_request_irq(&client->dev, irq1, gpio_irq_handler, 0, DEV_NAME, client);
        if (ret)
        {
            dev_err(&client->dev, "Failed to request irq1 %d: %d\n", irq1, ret);
            return ret;
        }
    }
    dev_info(&client->dev, "Requested irq %d\n", irq1);

    /* Register a platform device so a platform_driver can create a char device */
    pdev = platform_device_register_data(NULL, DEV_NAME, -1, &client, sizeof(client));
    if (IS_ERR(pdev))
    {
        ret = PTR_ERR(pdev);
        dev_err(&client->dev, "Failed to register platform device: %d\n", ret);
        return ret;
    }
    /* Save platform device pointer in client data so remove can unregister it */
    i2c_set_clientdata(client, pdev);

    ret = init_mpu6050(client);
    if (ret)
    {
        dev_err(&client->dev, "Failed to initialize driver: %d\n", ret);
        /* Clean up platform device on error */
        platform_device_unregister(pdev);
        i2c_set_clientdata(client, NULL);
        return ret;
    }

    dev_info(&client->dev, "Initialized (platform device registered)\n");
    return 0;
}

static void i2c_device_remove(struct i2c_client *client)
{
    remove_mpu6050(client);
    /* Unregister the platform device created in probe - this will trigger platform remove
     * where the char device is cleaned up. */
    {
        struct platform_device *pdev = i2c_get_clientdata(client);
        if (pdev)
            platform_device_unregister(pdev);
    }
    // No destroy needed for DEFINE_SPINLOCK() spinlocks
    dev_info(&client->dev, "Removed\n");
    return;
}

static const struct of_device_id device_of_match[] = {
    {.compatible = "arg,kdr_mpu6050"},
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, device_of_match);

static struct i2c_driver dev_i2c_driver = {
    .driver = {
        .name = DEV_NAME,
        .of_match_table = device_of_match,
    },
    .probe = i2c_device_probe,
    .remove = i2c_device_remove,
};

/* ========== Platform driver to create char device ========== */
static int mpu_platform_probe(struct platform_device *pdev)
{
    struct pdev_char_data *pdata;
    struct i2c_client *client = NULL;
    int ret;

    /* Retrieve i2c_client pointer passed by platform_device_register_data from i2c probe */
    client = *(struct i2c_client **)dev_get_platdata(&pdev->dev);
    if (!client)
    {
        dev_err(&pdev->dev, "No i2c client in platform data\n");
        return -ENODEV;
    }

    pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
    if (!pdata)
        return -ENOMEM;

    pdata->client = client;

    /* Allocate device number */
    ret = alloc_chrdev_region(&pdata->devno, 0, 1, DEV_NAME);
    if (ret)
    {
        dev_err(&pdev->dev, "alloc_chrdev_region failed: %d\n", ret);
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
    return ret;
}

static void mpu_platform_remove(struct platform_device *pdev)
{
    struct pdev_char_data *pdata = platform_get_drvdata(pdev);
    if (!pdata)
        return;

    device_destroy(pdata->class, pdata->devno);
    class_destroy(pdata->class);
    cdev_del(&pdata->cdev);
    unregister_chrdev_region(pdata->devno, 1);
    dev_info(&pdev->dev, "char device removed\n");
}

static struct platform_driver mpu_platform_driver = {
    .probe = mpu_platform_probe,
    .remove = mpu_platform_remove,
    .driver = {
        .name = DEV_NAME,
    },
};

/* Module init/exit register both drivers */
static int __init mpu_module_init(void)
{
    int ret;

    ret = platform_driver_register(&mpu_platform_driver);
    if (ret)
    {
        pr_err("Failed to register platform driver: %d\n", ret);
        return ret;
    }

    ret = i2c_add_driver(&dev_i2c_driver);
    if (ret)
    {
        pr_err("Failed to register i2c driver: %d\n", ret);
        platform_driver_unregister(&mpu_platform_driver);
        return ret;
    }

    pr_info("mpu driver initialized\n");
    return 0;
}

static void __exit mpu_module_exit(void)
{
    i2c_del_driver(&dev_i2c_driver);
    platform_driver_unregister(&mpu_platform_driver);
    pr_info("mpu driver exited\n");
}

module_init(mpu_module_init);
module_exit(mpu_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("MPU6050 with FIFO buffer enabled and checking IRQ status");
