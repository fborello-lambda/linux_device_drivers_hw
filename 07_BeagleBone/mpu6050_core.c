#include <linux/module.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
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
#define DEV_NAME "mpu6050"

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

    /* register misc device */
    ret = misc_register(&miscdevice_struct);
    if (ret)
    {
        dev_err(&client->dev, "Failed to register misc device: %d\n", ret);
        return ret;
    }

    ret = init_mpu6050(client);
    if (ret)
    {
        dev_err(&client->dev, "Failed to initialize driver: %d\n", ret);
        return ret;
    }

    dev_info(&client->dev, "Initialized (minor %d)\n", miscdevice_struct.minor);
    return 0;
}

static void i2c_device_remove(struct i2c_client *client)
{
    remove_mpu6050(client);
    misc_deregister(&miscdevice_struct);
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

module_i2c_driver(dev_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("MPU6050 with FIFO buffer enabled and checking IRQ status");
