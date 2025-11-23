#ifndef MPU6050_KDD_PRIMITIVES_H
#define MPU6050_KDD_PRIMITIVES_H

/*
 * mpu6050_kdd_primitives.h
 * Kernel Driver Development (KDD) Primitives for MPU6050
 */

#include <linux/types.h>
#include <linux/module.h>

#include "mpu6050_lib.h"
#include "i2c2_ll.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief Device structure for kernel driver development (KDD).
     *
     * Holds the I2C client, current scales and initialization state.
     */
    typedef struct
    {
        __u8 i2c_addr;
        mpu6050_accel_scale_t accel_scale;
        mpu6050_gyro_scale_t gyro_scale;
        __u8 initialized;
    } mpu6050_t;

    /** @brief Initializes the MPU-6050 device.
     *
     * Configures the device with the provided settings and prepares it for use.
     *
     * @param dev Pointer to mpu6050_t device structure.
     * @param cfg Configuration structure with desired settings.
     * @param client Pointer to initialized i2c_client structure.
     * @return MPU6050_OK on success, negative error code on failure.
     */
    int mpu6050_kdd_init(mpu6050_t *dev, mpu6050_config_full_t cfg);

    /** @brief Reads a byte from the specified register.
     *
     * @param dev Pointer to mpu6050_t device structure.
     * @param reg_val Pointer to variable to store the read register value.
     * @param reg Register address to read from.
     * @return MPU6050_OK on success, negative error code on failure.
     */
    int mpu6050_kdd_read_byte(mpu6050_t *dev, __u8 *reg_val, unsigned char reg);

    /** @brief Resets the MPU-6050 device.
     *
     * Performs a full device reset and reinitializes internal state.
     *
     * @param dev Pointer to mpu6050_t device structure.
     * @return MPU6050_OK on success, negative error code on failure.
     */
    int mpu6050_kdd_reset(mpu6050_t *dev);

    /** @brief Resets the FIFO buffer.
     *
     * Clears the FIFO buffer and resets its internal state.
     *
     * Usually called after a FIFO overflow condition.
     *
     * @param dev Pointer to mpu6050_t device structure.
     * @return MPU6050_OK on success, negative error code on failure.
     */
    int mpu6050_kdd_reset_fifo(mpu6050_t *dev);

    /** @brief Reads available samples from the FIFO buffer.
     *
     * High-level: read frames (14 bytes: accel+temp+gyro) into samples[]
     *
     * @param dev Pointer to mpu6050_t device structure.
     * @param samples Pointer to array to store the read samples.
     * @param max_samples Maximum number of samples to read.
     * @return Number of samples read on success, negative error code on failure.
     */
    ssize_t mpu6050_read_fifo_samples(mpu6050_t *dev,
                                      mpu6050_raw_t *samples,
                                      size_t max_samples);

    /** @brief Prints a formatted message with raw and fixed-point sample data.
     * @param buf Buffer to store the formatted message.
     * @param buflen Length of the buffer.
     * @param r Pointer to raw sample data.
     * @param fx Pointer to fixed-point sample data.
     * @param print_raw If non-zero, includes raw data in the output.
     * @param print_packed If non-zero, uses a more compact format.
     * @return Number of characters written to the buffer.
     */
    int mpu6050_kdd_print_msg(char *buf, int buflen, mpu6050_raw_t *r, mpu6050_sample_fixed_t *fx, __u8 print_raw, __u8 print_packed);

    /** @brief Converts raw sample data to fixed-point representation.
     *
     * Converts raw accelerometer, gyroscope, and temperature data to fixed-point
     * values in milli-g, milli-degrees per second, and milli-degrees Celsius,
     * respectively, based on the provided scale settings.
     *
     * @param dev Pointer to mpu6050_t device structure with current scale settings.
     * @param r Pointer to input raw sample data.
     * @param o Pointer to output fixed-point sample data.
     */
    inline static void mpu6050_kdd_raw_to_sample_fixed(mpu6050_t *dev, const mpu6050_raw_t *r,
                                                       mpu6050_sample_fixed_t *o)
    {
        mpu6050_accel_scale_t as = dev->accel_scale;
        mpu6050_gyro_scale_t gs = dev->gyro_scale;
        /* accel full-scale in g */
        int a_fs_g;
        switch (as)
        {
        case MPU6050_ACCEL_SCALE_2G:
            a_fs_g = 2;
            break;
        case MPU6050_ACCEL_SCALE_4G:
            a_fs_g = 4;
            break;
        case MPU6050_ACCEL_SCALE_8G:
            a_fs_g = 8;
            break;
        case MPU6050_ACCEL_SCALE_16G:
            a_fs_g = 16;
            break;
        default:
            a_fs_g = 2;
        }

        /* gyro full-scale in dps */
        int g_fs_dps;
        switch (gs)
        {
        case MPU6050_GYRO_SCALE_250:
            g_fs_dps = 250;
            break;
        case MPU6050_GYRO_SCALE_500:
            g_fs_dps = 500;
            break;
        case MPU6050_GYRO_SCALE_1000:
            g_fs_dps = 1000;
            break;
        case MPU6050_GYRO_SCALE_2000:
            g_fs_dps = 2000;
            break;
        default:
            g_fs_dps = 250;
        }

        /* Use 64-bit to avoid overflow: raw * fs * 1000 */
        o->ax_mg = (int)(((s64)r->ax * a_fs_g * 1000) / 32768);
        o->ay_mg = (int)(((s64)r->ay * a_fs_g * 1000) / 32768);
        o->az_mg = (int)(((s64)r->az * a_fs_g * 1000) / 32768);

        o->gx_mdps = (int)(((s64)r->gx * g_fs_dps * 1000) / 32768);
        o->gy_mdps = (int)(((s64)r->gy * g_fs_dps * 1000) / 32768);
        o->gz_mdps = (int)(((s64)r->gz * g_fs_dps * 1000) / 32768);

        /* Temp: (raw / 340) + 36.53  -> milli-degC */
        o->temp_mdegC = (int)(((s64)r->temp * 1000) / 340 + 36530);
    }

    /** @brief Macro to convert a byte value to a binary string (for debug output).
     *
     * Usage:
     * ```c
     * char bin[12];
     * TO_BIN(value, bin);
     * pr_info("Value is %s\n", bin);
     * ```
     * Resulting string format: "0bXXXX_XXXX"
     */
#define TO_BIN(v, buf)                                             \
    do                                                             \
    {                                                              \
        int __i;                                                   \
        (buf)[0] = '0';                                            \
        (buf)[1] = 'b';                                            \
        for (__i = 0; __i < 4; ++__i)                              \
            (buf)[2 + __i] = ((v) & (1 << (7 - __i))) ? '1' : '0'; \
        (buf)[6] = '_';                                            \
        for (__i = 0; __i < 4; ++__i)                              \
            (buf)[7 + __i] = ((v) & (1 << (3 - __i))) ? '1' : '0'; \
        (buf)[11] = '\0';                                          \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* MPU6050_KDD_PRIMITIVES_H */
