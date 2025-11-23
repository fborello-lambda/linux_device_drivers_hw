#include "mpu6050_kdd_primitives.h"
#include <linux/delay.h>     // msleep/usleep_range
#include <linux/sched.h>     // schedule_timeout, TASK_*
#include <linux/unaligned.h> // get_unaligned_be16
#include <linux/slab.h>      // kmalloc/kfree

static int mpu6050_write_reg(mpu6050_t *dev, __u8 reg, __u8 val)
{
    int ret;
    if (!dev)
        return MPU6050_ERR;
    ret = i2c2_ll_write_byte(dev->i2c_addr, reg, val);
    return (ret < 0) ? ret : MPU6050_OK;
}

int mpu6050_kdd_init(mpu6050_t *dev, mpu6050_config_full_t cfg)
{
    if (!dev)
        return MPU6050_ERR_BAD_PARAM;

    // Configure default scales
    dev->accel_scale = cfg.accel_scale;
    dev->gyro_scale = cfg.gyro_scale;

    // Reset device
    pr_info("MPU6050: Performing device reset\n");
    if (mpu6050_kdd_reset(dev) != MPU6050_OK)
        return MPU6050_ERR;

    /* Ensure the device is awake and has a valid clock source after reset.
     * Select X-axis PLL (0x01) which is a recommended stable clock source.
     * This clears the SLEEP bit so the sensor can produce data/interrupts.
     *
     * According to the datasheet if CLKSEL is 0 it's configured to use the internal 8MHz oscillator,
     * but seems that it doesn't work properly, at least in my tests.
     * Maybe it has to do with DPLF settings or something else.
     */
    if (mpu6050_write_reg(dev, MPU6050_REG_PWR_MGMT_1, 0x01) != MPU6050_OK)
        return MPU6050_ERR;

    // Set scales, filters, enable fifo and interrupts
    char bin[12];

    TO_BIN(dev->accel_scale, bin);
    pr_info("MPU6050: Setting up device with accel scale %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_ACCEL_CONFIG, dev->accel_scale) != MPU6050_OK)
        return MPU6050_ERR;

    TO_BIN(dev->gyro_scale, bin);
    pr_info("MPU6050: Setting up device with gyro scale %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_GYRO_CONFIG, dev->gyro_scale) != MPU6050_OK)
        return MPU6050_ERR;

    TO_BIN(cfg.dlpf_cfg, bin);
    pr_info("MPU6050: Setting up device with DLPF config %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_CONFIG, cfg.dlpf_cfg) != MPU6050_OK)
        return MPU6050_ERR;

    TO_BIN(cfg.sample_rate_div, bin);
    pr_info("MPU6050: Setting up device with sample rate divider %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_SMPLRT_DIV, cfg.sample_rate_div) != MPU6050_OK)
        return MPU6050_ERR;

    TO_BIN(cfg.fifo_en, bin);
    pr_info("MPU6050: Setting up device with FIFO enable %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_FIFO_EN, cfg.fifo_en) != MPU6050_OK)
        return MPU6050_ERR;

    TO_BIN(cfg.int_pin_cfg, bin);
    pr_info("MPU6050: Setting up device with INT pin config %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_INT_PIN_CFG, cfg.int_pin_cfg) != MPU6050_OK)
        return MPU6050_ERR;

    TO_BIN(cfg.int_enable, bin);
    pr_info("MPU6050: Setting up device with INT enable %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_INT_ENABLE, cfg.int_enable) != MPU6050_OK)
        return MPU6050_ERR;

    /* Clear any pending interrupt flags by reading INT_STATUS. */
    {
        int __st = i2c2_ll_read_byte(dev->i2c_addr, MPU6050_REG_INT_STATUS);
        TO_BIN(__st, bin);
        if (__st >= 0)
            pr_debug("MPU6050: INT_STATUS cleared (0x%02x)\n", __st);
    }

    TO_BIN(cfg.user_ctrl, bin);
    pr_info("MPU6050: Setting up device with USER control %s", bin);
    if (mpu6050_write_reg(dev, MPU6050_REG_USER_CTRL, cfg.user_ctrl) != MPU6050_OK)
        return MPU6050_ERR;

    pr_info("MPU6050: Initialization complete\n");
    dev->initialized = 1;
    return MPU6050_OK;
}

/* Get FIFO byte count */
static int mpu6050_kdd_get_fifo_count(mpu6050_t *dev, unsigned int *count)
{
    int hi, lo;

    if (!dev->initialized)
        return MPU6050_ERR_NOT_INITIALIZED;

    if (!dev || !count)
        return -EINVAL;

    // Note: Reading only FIFO_COUNT_L will not update the registers to the current sample count.
    // FIFO_COUNT_H must be accessed first to update the contents of both these registers.
    hi = i2c2_ll_read_byte(dev->i2c_addr, MPU6050_REG_FIFO_COUNTH);
    if (hi < 0)
        return hi;
    lo = i2c2_ll_read_byte(dev->i2c_addr, MPU6050_REG_FIFO_COUNTL);
    if (lo < 0)
        return lo;

    *count = ((hi & 0xff) << 8) | (lo & 0xff);
    return 0;
}

/* Low-level: read exactly req bytes (or less on short read) from FIFO (no count query). */
static ssize_t mpu6050_read_fifo_bytes(mpu6050_t *dev, u8 *buf, size_t req)
{
    size_t remaining = req;
    size_t offset = 0;
    int ret;

    if (!dev->initialized)
        return MPU6050_ERR_NOT_INITIALIZED;

    if (!dev || !buf || req == 0)
        return -EINVAL;

    while (remaining > 0)
    {
        size_t chunk = (remaining > 32) ? 32 : remaining;

        ret = i2c2_ll_read_block(dev->i2c_addr,
                                 MPU6050_REG_FIFO_R_W,
                                 (u8)chunk,
                                 &buf[offset]);
        if (ret < 0)
            return ret;
        if (ret == 0)
            break; /* unexpected */

        offset += ret;
        remaining -= ret;

        if ((size_t)ret != chunk)
            break; /* short read */
    }
    return offset;
}

/* High-level: read frames (14 bytes: accel+temp+gyro) into samples[] */
ssize_t mpu6050_read_fifo_samples(mpu6050_t *dev,
                                  mpu6050_raw_t *samples,
                                  size_t max_samples)
{
    unsigned int fifo_bytes;
    size_t frame = 14;
    size_t frames, want_bytes;
    u8 *tmp;
    ssize_t got;
    size_t i;
    int rc;

    if (!dev->initialized)
        return MPU6050_ERR_NOT_INITIALIZED;

    if (!dev || !samples || max_samples == 0)
        return -EINVAL;

    rc = mpu6050_kdd_get_fifo_count(dev, &fifo_bytes);
    pr_debug("MPU6050: FIFO count = %u bytes\n", fifo_bytes);
    if (rc < 0)
        return rc;
    if (fifo_bytes < frame)
        return 0;

    frames = fifo_bytes / frame;
    if (frames > max_samples)
        frames = max_samples;
    want_bytes = frames * frame;

    // Using GFP_ATOMIC because this function may be called from IRQ context
    // (threaded IRQ handler).
    tmp = kmalloc(want_bytes, GFP_ATOMIC);
    if (!tmp)
        return -ENOMEM;

    got = mpu6050_read_fifo_bytes(dev, tmp, want_bytes);
    pr_debug("MPU6050: read %zd bytes from FIFO\n", got);
    if (got < 0)
    {
        kfree(tmp);
        return got;
    }
    if ((size_t)got < frame)
    {
        kfree(tmp);
        return 0;
    }
    if ((size_t)got < want_bytes)
        frames = (size_t)got / frame;

    for (i = 0; i < frames; i++)
    {
        size_t off = i * frame;
        samples[i].ax = (short)get_unaligned_be16(&tmp[off + 0]);
        samples[i].ay = (short)get_unaligned_be16(&tmp[off + 2]);
        samples[i].az = (short)get_unaligned_be16(&tmp[off + 4]);
        samples[i].temp = (short)get_unaligned_be16(&tmp[off + 6]);
        samples[i].gx = (short)get_unaligned_be16(&tmp[off + 8]);
        samples[i].gy = (short)get_unaligned_be16(&tmp[off + 10]);
        samples[i].gz = (short)get_unaligned_be16(&tmp[off + 12]);
    }

    pr_debug("MPU6050: %u frames converted to samples\n", frames);

    kfree(tmp);
    return frames;
}

int mpu6050_kdd_read_byte(mpu6050_t *dev, __u8 *reg_val, unsigned char reg)
{
    if (!dev)
        return MPU6050_ERR;

    int ret = i2c2_ll_read_byte(dev->i2c_addr, reg);
    if (ret < 0)
    {
        pr_err("MPU6050: %X read failed (ret=%d)\n", reg, ret);
        return MPU6050_ERR;
    }
    
    *reg_val = (__u8)ret;
    return MPU6050_OK;
}

int mpu6050_kdd_reset_fifo(mpu6050_t *dev)
{
    if (!dev)
        return MPU6050_ERR;

    // From the datasheet:
    // This bit resets the FIFO buffer when set to 1 while FIFO_EN equals 0. This
    // bit automatically clears to 0 after the reset has been triggered.
    if (mpu6050_write_reg(dev, MPU6050_REG_USER_CTRL, 0) != MPU6050_OK)
        return MPU6050_ERR;
    if (mpu6050_write_reg(dev, MPU6050_REG_USER_CTRL, MPU6050_USERCTRL_FIFO_RESET) != MPU6050_OK)
        return MPU6050_ERR;
    if (mpu6050_write_reg(dev, MPU6050_REG_USER_CTRL, MPU6050_USERCTRL_FIFO_EN) != MPU6050_OK)
        return MPU6050_ERR;

    return MPU6050_OK;
}

int mpu6050_kdd_reset(mpu6050_t *dev)
{
    if (!dev)
        return MPU6050_ERR;

    mpu6050_kdd_reset_fifo(dev);

    int ret = mpu6050_write_reg(dev, MPU6050_REG_PWR_MGMT_1, 0b10000000);
    if (ret < 0)
        return MPU6050_ERR;

    /* Give the sensor time to complete internal reset */
    /**
     * I don't know if this is the best way of doing this.
     * Maybe just sleep is enough. Changing task state to interruptible
     * and using schedule_timeout() is probably overkill.
     */
    set_current_state(TASK_INTERRUPTIBLE);
    schedule_timeout(msecs_to_jiffies(120));
    set_current_state(TASK_RUNNING);

    /* Reset the signal path, recommended in the datasheet */
    if (mpu6050_write_reg(dev, MPU6050_REG_SIGNAL_PATH_RESET, MPU6050_SIGNAL_PATH_RESET_ALL) != MPU6050_OK)
        return MPU6050_ERR;

    set_current_state(TASK_INTERRUPTIBLE);
    schedule_timeout(msecs_to_jiffies(120));
    set_current_state(TASK_RUNNING);

    return MPU6050_OK;
}

int mpu6050_kdd_print_msg(char *buf, int buflen, mpu6050_raw_t *r, mpu6050_sample_fixed_t *fx, __u8 print_raw, __u8 print_packed)
{
    /** Cannot use floating point in kernel code
     *
     * error: ‘-mgeneral-regs-only’ is incompatible with the use of floating-point types
     *
     * print using fixed-point (millis) values without fp ops,
     * keep sign for negative values
     */
    int ax_v = fx->ax_mg, ay_v = fx->ay_mg, az_v = fx->az_mg;
    int gx_v = fx->gx_mdps, gy_v = fx->gy_mdps, gz_v = fx->gz_mdps;
    int tmp_v = fx->temp_mdegC;

    const char *ax_sign = (ax_v < 0) ? "-" : " ";
    const char *ay_sign = (ay_v < 0) ? "-" : " ";
    const char *az_sign = (az_v < 0) ? "-" : " ";
    const char *gx_sign = (gx_v < 0) ? "-" : " ";
    const char *gy_sign = (gy_v < 0) ? "-" : " ";
    const char *gz_sign = (gz_v < 0) ? "-" : " ";
    const char *tmp_sign = (tmp_v < 0) ? "-" : "";

    int ax_wh = ax_v / 1000;
    int ax_fr = ax_v % 1000;
    if (ax_fr < 0)
        ax_fr = -ax_fr;
    int ay_wh = ay_v / 1000;
    int ay_fr = ay_v % 1000;
    if (ay_fr < 0)
        ay_fr = -ay_fr;
    int az_wh = az_v / 1000;
    int az_fr = az_v % 1000;
    if (az_fr < 0)
        az_fr = -az_fr;

    int gx_wh = gx_v / 1000;
    int gx_fr = gx_v % 1000;
    if (gx_fr < 0)
        gx_fr = -gx_fr;
    int gy_wh = gy_v / 1000;
    int gy_fr = gy_v % 1000;
    if (gy_fr < 0)
        gy_fr = -gy_fr;
    int gz_wh = gz_v / 1000;
    int gz_fr = gz_v % 1000;
    if (gz_fr < 0)
        gz_fr = -gz_fr;

    int tmp_wh = tmp_v / 1000;
    int tmp_fr = tmp_v % 1000;
    if (tmp_fr < 0)
        tmp_fr = -tmp_fr;

    int pos = 0;

    if (print_raw)
    {
        pos += scnprintf(buf + pos, buflen - pos,
                         "RAW ax=%d ay=%d az=%d gx=%d gy=%d gz=%d temp=%d\n",
                         r->ax, r->ay, r->az, r->gx, r->gy, r->gz, r->temp);
    }
    if (print_packed)
    {
        // Easier to split and parse
        // x,y,z (Acceleration [g])
        // x,y,z (Gyro [dps])
        // temp [C]
        pos += scnprintf(buf + pos, buflen - pos,
                         "%s%d.%03d,%s%d.%03d,%s%d.%03d, [g]\n"
                         "%s%d.%03d,%s%d.%03d,%s%d.%03d, [dps]\n"
                         "%s%d.%03d, [°C]\n",
                         ax_sign, (ax_wh < 0) ? -ax_wh : ax_wh, ax_fr,
                         ay_sign, (ay_wh < 0) ? -ay_wh : ay_wh, ay_fr,
                         az_sign, (az_wh < 0) ? -az_wh : az_wh, az_fr,
                         gx_sign, (gx_wh < 0) ? -gx_wh : gx_wh, gx_fr,
                         gy_sign, (gy_wh < 0) ? -gy_wh : gy_wh, gy_fr,
                         gz_sign, (gz_wh < 0) ? -gz_wh : gz_wh, gz_fr,
                         tmp_sign, (tmp_wh < 0) ? -tmp_wh : tmp_wh, tmp_fr);
    }
    else
    {
        pos += scnprintf(buf + pos, buflen - pos,
                         "ax=%s%d.%03d g\nay=%s%d.%03d g\naz=%s%d.%03d g\n"
                         "gx=%s%d.%03d dps\ngy=%s%d.%03d dps\ngz=%s%d.%03d dps\n"
                         "temp=%s%d.%03d C\n",
                         ax_sign, (ax_wh < 0) ? -ax_wh : ax_wh, ax_fr,
                         ay_sign, (ay_wh < 0) ? -ay_wh : ay_wh, ay_fr,
                         az_sign, (az_wh < 0) ? -az_wh : az_wh, az_fr,
                         gx_sign, (gx_wh < 0) ? -gx_wh : gx_wh, gx_fr,
                         gy_sign, (gy_wh < 0) ? -gy_wh : gy_wh, gy_fr,
                         gz_sign, (gz_wh < 0) ? -gz_wh : gz_wh, gz_fr,
                         tmp_sign, (tmp_wh < 0) ? -tmp_wh : tmp_wh, tmp_fr);
    }

    return pos;
}
