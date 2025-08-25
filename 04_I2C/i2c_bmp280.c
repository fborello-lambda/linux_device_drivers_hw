#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>

#define DEV_NAME "i2c_bmp280"
#define I2C_BUS 1
#define I2C_ADDR 0x77

static struct i2c_client *bmp280_client = NULL;

/* ======== I2C driver integration ======== */
typedef struct
{
    u16 dig_T1;
    s16 dig_T2;
    s16 dig_T3;
    u16 dig_P1;
    s16 dig_P2;
    s16 dig_P3;
    s16 dig_P4;
    s16 dig_P5;
    s16 dig_P6;
    s16 dig_P7;
    s16 dig_P8;
    s16 dig_P9;
} calib_data_t;

typedef struct
{
    u8 press_msb;  // 0xF7
    u8 press_lsb;  // 0xF8
    u8 press_xlsb; // 0xF9 (4 upper bits of register | 7,6,5,4)
    u8 temp_msb;   // 0xFA
    u8 temp_lsb;   // 0xFB
    u8 temp_xlsb;  // 0xFC s(4 upper bits of register | 7,6,5,4)
} raw_data_t;

typedef struct
{
    int32_t temp;
    uint32_t press;
} bmp280_data_t;

calib_data_t calib_data;

static int init_bmp280(struct i2c_client *client)
{
    int ret;
    u8 buf[24];
    u8 ctrl_meas = 0x6F; // Temp and Press oversampling x4, mode normal
    u8 config = 0x90;

    /* Read the chip ID */
    ret = i2c_smbus_read_byte_data(client, 0xD0);
    if (ret < 0)
        return dev_err_probe(&client->dev, ret, "Failed to read chip ID\n");

    // ret == chip_id
    if (ret != 0x58) // expected BMP280 ID
        return dev_err_probe(&client->dev, -ENODEV,
                             "Unexpected chip ID: 0x%02x\n", ret);

    dev_info(&client->dev, "BMP280 detected with chip ID: 0x%02x\n", ret);

    /* Configure registers */
    if ((ret = i2c_smbus_write_byte_data(client, 0xF4, ctrl_meas)) < 0)
        return dev_err_probe(&client->dev, ret, "Failed to write ctrl_meas\n");

    if ((ret = i2c_smbus_write_byte_data(client, 0xF5, config)) < 0)
        return dev_err_probe(&client->dev, ret, "Failed to write config\n");

    /* Read calibration data (0x88..0xA1) */
    ret = i2c_smbus_read_i2c_block_data(client, 0x88, sizeof(buf), buf);
    if (ret < 0)
        return dev_err_probe(&client->dev, ret, "Failed to read calibration data\n");
    if (ret != sizeof(buf))
        return dev_err_probe(&client->dev, -EIO, "Incomplete calib read (%d)\n", ret);

    /* Parse little-endian calibration values */
    calib_data.dig_T1 = le16_to_cpup((__le16 *)&buf[0]);
    calib_data.dig_T2 = le16_to_cpup((__le16 *)&buf[2]);
    calib_data.dig_T3 = le16_to_cpup((__le16 *)&buf[4]);
    calib_data.dig_P1 = le16_to_cpup((__le16 *)&buf[6]);
    calib_data.dig_P2 = le16_to_cpup((__le16 *)&buf[8]);
    calib_data.dig_P3 = le16_to_cpup((__le16 *)&buf[10]);
    calib_data.dig_P4 = le16_to_cpup((__le16 *)&buf[12]);
    calib_data.dig_P5 = le16_to_cpup((__le16 *)&buf[14]);
    calib_data.dig_P6 = le16_to_cpup((__le16 *)&buf[16]);
    calib_data.dig_P7 = le16_to_cpup((__le16 *)&buf[18]);
    calib_data.dig_P8 = le16_to_cpup((__le16 *)&buf[20]);
    calib_data.dig_P9 = le16_to_cpup((__le16 *)&buf[22]);

    dev_info(&client->dev, "Calibration data loaded\n");
    dev_info(&client->dev, "dig_T1: %u, dig_T2: %d, dig_T3: %d\n",
             calib_data.dig_T1, calib_data.dig_T2, calib_data.dig_T3);

    return 0;
}

static int remove_bmp280(struct i2c_client *client)
{
    int ret;
    if ((ret = i2c_smbus_write_byte_data(client, 0xE0, 0xB6)) < 0)
    {
        dev_err(&client->dev, "Failed to reset BMP280: %d\n", ret);
        return ret;
    }

    return 0;
}

static int read_raw_data(struct i2c_client *client, raw_data_t *raw)
{
    int ret;
    u8 buf[6];

    ret = i2c_smbus_read_i2c_block_data(client, 0xF7, sizeof(buf), buf);
    if (ret < 0)
        return dev_err_probe(&client->dev, ret, "Failed to read raw data\n");
    if (ret != sizeof(buf))
        return dev_err_probe(&client->dev, -EIO, "Incomplete raw data read (%d)\n", ret);

    raw->press_msb = buf[0];
    raw->press_lsb = buf[1];
    raw->press_xlsb = (buf[2] & 0xF0) >> 4;
    raw->temp_msb = buf[3];
    raw->temp_lsb = buf[4];
    raw->temp_xlsb = (buf[5] & 0xF0) >> 4;

    return 0;
}

// Section 3.11.3 of the datasheet
static int compensate_data(raw_data_t *raw, bmp280_data_t *data)
{
    int32_t adc_T, adc_P;
    int32_t var1, var2, t_fine;
    int64_t varp1, varp2;

    adc_T = (raw->temp_msb << 12) | (raw->temp_lsb << 4) | raw->temp_xlsb;
    adc_P = (raw->press_msb << 12) | (raw->press_lsb << 4) | raw->press_xlsb;

    // Temperature compensation
    var1 = ((((adc_T >> 3) - ((int32_t)calib_data.dig_T1 << 1))) * ((int32_t)calib_data.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)calib_data.dig_T1)) * ((adc_T >> 4) - ((int32_t)calib_data.dig_T1))) >> 12) * ((int32_t)calib_data.dig_T3)) >> 14;
    t_fine = var1 + var2;
    data->temp = (t_fine * 5 + 128) >> 8;

    // Pressure compensation
    // It reads the temperature first to get t_fine
    varp1 = ((int64_t)t_fine) - 128000;
    varp2 = varp1 * varp1 * (int64_t)calib_data.dig_P6;
    varp2 = varp2 + ((varp1 * (int64_t)calib_data.dig_P5) << 17);
    varp2 = varp2 + (((int64_t)calib_data.dig_P4) << 35);
    varp1 = ((varp1 * varp1 * (int64_t)calib_data.dig_P3) >> 8) + ((varp1 * (int64_t)calib_data.dig_P2) << 12);
    varp1 = (((((int64_t)1) << 47) + varp1)) * ((int64_t)calib_data.dig_P1) >> 33;

    if (varp1 == 0)
    {
        // Avoid division by zero
        // Set pressure to zero and assume temperature is valid
        data->press = 0;
        return 0;
    }
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - varp2) * 3125) / varp1;
    varp1 = (((int64_t)calib_data.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    varp2 = (((int64_t)calib_data.dig_P8) * p) >> 19;
    p = ((p + varp1 + varp2) >> 8) + (((int64_t)calib_data.dig_P7) << 4);
    data->press = (uint32_t)p;

    return 0;
}

/* ======== fops device integration ======== */
static ssize_t dev_read(struct file *file, char __user *buf,
                        size_t count, loff_t *ppos)
{
    raw_data_t raw;
    bmp280_data_t data_out;
    int ret;
    char kbuf[64];
    int len;

    if (bmp280_client == NULL)
        return -ENODEV;

    ret = read_raw_data(bmp280_client, &raw);
    if (ret)
        return ret;

    ret = compensate_data(&raw, &data_out);
    if (ret)
        return ret;

    /* Format data into kernel buffer */
    len = snprintf(kbuf, sizeof(kbuf),
                   "Temp: %d.%02d °C, Press: %d.%02d hPa\n",
                   data_out.temp / 100, data_out.temp % 100,
                   data_out.press / 25600,
                   (data_out.press % 25600) * 100 / 25600);

    if (*ppos >= len)
        return 0;

    if (count > len - *ppos)
        count = len - *ppos;

    if (copy_to_user(buf, kbuf + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static const struct file_operations dev_fops = {
    .owner = THIS_MODULE,
    .read = dev_read,
};

/* ======== MISC device integration ======== */
/* Use misc device to let kernel/udev create /dev entry automatically */
static struct miscdevice dev_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEV_NAME,
    .mode = 0666,
    .fops = &dev_fops,
};

static int dev_probe(struct i2c_client *client)
{
    int ret;

    // NOT IDEAL, BUT SIMPLE
    // The client is needed in the dev_read() function
    // A better way would be to allocate a private structure
    /*
    Something like this:
        struct bmp280_dev {
            struct i2c_client *client;
            calib_data_t calib_data;
            struct miscdevice miscdev;
        };
    And then allocate it in dev_probe() and free it in dev_remove()
    */
    bmp280_client = client;

    ret = init_bmp280(client);
    if (ret)
    {
        dev_err(&client->dev, "Failed to initialize BMP280: %d\n", ret);
        return ret;
    }

    if (ret)
        return ret;

    ret = misc_register(&dev_misc);
    if (ret)
        return ret;

    dev_info(&client->dev, "%s: initialized (minor %d)\n", DEV_NAME, dev_misc.minor);
    return 0;
}

static void dev_remove(struct i2c_client *client)
{
    remove_bmp280(client);
    misc_deregister(&dev_misc);
    dev_info(&client->dev, "%s: removed\n", DEV_NAME);
    return;
}

static const struct of_device_id dev_misc_of_match[] = {
    {.compatible = "arg,i2c_bmp280"},
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, dev_misc_of_match);

static struct i2c_driver dev_i2c_driver = {
    .driver = {
        .name = DEV_NAME,
        .of_match_table = dev_misc_of_match,
    },
    .probe = dev_probe,
    .remove = dev_remove,
};

module_i2c_driver(dev_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("Simple I2C Driver for the BMP280 sensor");
