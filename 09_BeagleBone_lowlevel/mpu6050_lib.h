/*
 * mpu6050_lib.h
 *
 * MPU-6050 registers, types and helper functions.
 *
 * Used in both user-space and kernel-space code.
 */

#ifndef MPU6050_H_
#define MPU6050_H_

#ifdef __cplusplus
extern "C"
{
#endif

/* I2C 7-bit default address for MPU-6050 */
#define MPU6050_I2C_ADDR_DEFAULT 0x68u

/* Common registers (not exhaustive) */
#define MPU6050_REG_SMPLRT_DIV 0x19
#define MPU6050_REG_CONFIG 0x1A
#define MPU6050_REG_GYRO_CONFIG 0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_FIFO_EN 0x23
#define MPU6050_REG_INT_PIN_CFG 0x37
#define MPU6050_REG_INT_ENABLE 0x38
#define MPU6050_REG_INT_STATUS 0x3A
// begin data registers
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_ACCEL_XOUT_L 0x3C
#define MPU6050_REG_ACCEL_YOUT_H 0x3D
#define MPU6050_REG_ACCEL_YOUT_L 0x3E
#define MPU6050_REG_ACCEL_ZOUT_H 0x3F
#define MPU6050_REG_ACCEL_ZOUT_L 0x40
#define MPU6050_REG_TEMP_OUT_H 0x41
#define MPU6050_REG_TEMP_OUT_L 0x42
#define MPU6050_REG_GYRO_XOUT_H 0x43
#define MPU6050_REG_GYRO_XOUT_L 0x44
#define MPU6050_REG_GYRO_YOUT_H 0x45
#define MPU6050_REG_GYRO_YOUT_L 0x46
#define MPU6050_REG_GYRO_ZOUT_H 0x47
#define MPU6050_REG_GYRO_ZOUT_L 0x48
// end data registers
#define MPU6050_REG_SIGNAL_PATH_RESET 0x68
#define MPU6050_REG_USER_CTRL 0x6A
#define MPU6050_REG_PWR_MGMT_1 0x6B
#define MPU6050_REG_FIFO_COUNTH 0x72
#define MPU6050_REG_FIFO_COUNTL 0x73
#define MPU6050_REG_FIFO_R_W 0x74
#define MPU6050_REG_WHO_AM_I 0x75

    /* Configuration enum */
    typedef enum
    {
        MPU6050_DLPF_CF_260HZ = 0, // Accelerometer: 260Hz, Gyroscope: 256Hz Fs: 8kHz
        MPU6050_DLPF_CF_184HZ = 1, // Accelerometer: 184Hz, Gyroscope: 188Hz Fs: 1kHz
        MPU6050_DLPF_CF_94HZ = 2,  // Accelerometer: 94Hz, Gyroscope: 98Hz Fs: 1kHz
        MPU6050_DLPF_CF_44HZ = 3,  // Accelerometer: 44Hz, Gyroscope: 42Hz Fs: 1kHz
        MPU6050_DLPF_CF_21HZ = 4,  // Accelerometer: 21Hz, Gyroscope: 20Hz Fs: 1kHz
        MPU6050_DLPF_CF_10HZ = 5,  // Accelerometer: 10Hz, Gyroscope: 10Hz Fs: 1kHz
        MPU6050_DLPF_CF_5HZ = 6    // Accelerometer: 5Hz, Gyroscope: 5Hz Fs: 1kHz
    } mpu6050_config_t;

    /* Gyroscope configuration enum */
    typedef enum
    {
        MPU6050_GYRO_SCALE_250 = 0 << 3,  /* ±250 °/s */
        MPU6050_GYRO_SCALE_500 = 1 << 3,  /* ±500 °/s */
        MPU6050_GYRO_SCALE_1000 = 2 << 3, /* ±1000 °/s */
        MPU6050_GYRO_SCALE_2000 = 3 << 3  /* ±2000 °/s */
    } mpu6050_gyro_scale_t;

    /* Accelerometer configuration enum */
    typedef enum
    {
        MPU6050_ACCEL_SCALE_2G = 0 << 3, /* ±2g */
        MPU6050_ACCEL_SCALE_4G = 1 << 3, /* ±4g */
        MPU6050_ACCEL_SCALE_8G = 2 << 3, /* ±8g */
        MPU6050_ACCEL_SCALE_16G = 3 << 3 /* ±16g */
    } mpu6050_accel_scale_t;

    /* FIFO enable bits */
    typedef enum
    {
        MPU6050_FIFO_DISABLED = 0,
        MPU6050_FIFO_TEMP = 1 << 7,
        MPU6050_FIFO_GYRO_X = 1 << 6,
        MPU6050_FIFO_GYRO_Y = 1 << 5,
        MPU6050_FIFO_GYRO_Z = 1 << 4,
        MPU6050_FIFO_ACCEL = 1 << 3,
        MPU6050_FIFO_SLV2 = 1 << 2,
        MPU6050_FIFO_SLV1 = 1 << 1,
        MPU6050_FIFO_SLV0 = 1 << 0
    } mpu6050_fifo_en_t;

#define MPU6050_FIFO_ENABLE_ALL (MPU6050_FIFO_TEMP | MPU6050_FIFO_GYRO_X | MPU6050_FIFO_GYRO_Y | MPU6050_FIFO_GYRO_Z | MPU6050_FIFO_ACCEL)

/* INT_PIN_CFG (0x37) bit positions */
#define MPU6050_INT_LEVEL_BIT 7
#define MPU6050_INT_OPEN_BIT 6
#define MPU6050_LATCH_INT_EN_BIT 5
#define MPU6050_INT_RD_CLEAR_BIT 4
#define MPU6050_FSYNC_INT_LEVEL_BIT 3
#define MPU6050_FSYNC_INT_EN_BIT 2
#define MPU6050_I2C_BYPASS_EN_BIT 1

    typedef enum
    {
        MPU6050_INT_LEVEL_ACTIVE_HIGH = 0,
        MPU6050_INT_LEVEL_ACTIVE_LOW = 1
    } mpu6050_int_level_t;

    typedef enum
    {
        MPU6050_INT_OPEN_DRAIN = 1,
        MPU6050_INT_PUSH_PULL = 0
    } mpu6050_int_open_t;

    typedef enum
    {
        MPU6050_INT_LATCHED = 1,
        MPU6050_INT_PULSE = 0
    } mpu6050_int_latch_int_en_t;

    typedef enum
    {
        MPU6050_INT_RD_CLEAR_ANY = 1,
        MPU6050_INT_RD_CLEAR_BY_READING_INT_STATUS = 0
    } mpu6050_int_rd_clear_t;

    typedef char mpu6050_int_pin_cfg_t;

    /* Usage
    ```c
        mpu6050_int_pin_cfg_t cfg = mpu6050_int_pin_cfg_pack(
                                MPU6050_INT_LEVEL_ACTIVE_LOW,   // INT level
                                MPU6050_INT_PUSH_PULL,          // push-pull/open-drain
                                MPU6050_INT_LATCHED,            // latched vs pulse
                                true,                           // INT_RD_CLEAR = 1 -> clear on any read
                                MPU6050_INT_LEVEL_ACTIVE_LOW,   // FSYNC active level
                                false,                          // FSYNC_INT_EN
                                false                           // I2C_BYPASS_EN
                                );
    ```
    */
    static inline mpu6050_int_pin_cfg_t
    mpu6050_int_pin_cfg_pack(mpu6050_int_level_t level,
                             mpu6050_int_open_t open_type,
                             mpu6050_int_latch_int_en_t latch,
                             mpu6050_int_rd_clear_t int_rd_clear,
                             mpu6050_int_level_t fsync_level,
                             char fsync_en,
                             char i2c_bypass)
    {
        mpu6050_int_pin_cfg_t v = 0;
        v |= ((level & 1) << MPU6050_INT_LEVEL_BIT);
        v |= ((open_type & 1) << MPU6050_INT_OPEN_BIT);
        v |= ((latch & 1) << MPU6050_LATCH_INT_EN_BIT);
        v |= ((int_rd_clear ? 1 : 0) << MPU6050_INT_RD_CLEAR_BIT);
        v |= ((fsync_level & 1) << MPU6050_FSYNC_INT_LEVEL_BIT);
        v |= ((fsync_en ? 1 : 0) << MPU6050_FSYNC_INT_EN_BIT);
        v |= ((i2c_bypass ? 1 : 0) << MPU6050_I2C_BYPASS_EN_BIT);
        return v;
    }

    static inline void
    mpu6050_int_pin_cfg_unpack(mpu6050_int_pin_cfg_t v,
                               mpu6050_int_level_t *level,
                               mpu6050_int_open_t *open_type,
                               mpu6050_int_latch_int_en_t *latch,
                               char *int_rd_clear,
                               mpu6050_int_level_t *fsync_level,
                               char *fsync_en,
                               char *i2c_bypass)
    {
        if (level)
            *level = (mpu6050_int_level_t)((v >> MPU6050_INT_LEVEL_BIT) & 1);
        if (open_type)
            *open_type = (mpu6050_int_open_t)((v >> MPU6050_INT_OPEN_BIT) & 1);
        if (latch)
            *latch = (mpu6050_int_latch_int_en_t)((v >> MPU6050_LATCH_INT_EN_BIT) & 1);
        if (int_rd_clear)
            *int_rd_clear = !!((v >> MPU6050_INT_RD_CLEAR_BIT) & 1);
        if (fsync_level)
            *fsync_level = (mpu6050_int_level_t)((v >> MPU6050_FSYNC_INT_LEVEL_BIT) & 1);
        if (fsync_en)
            *fsync_en = !!((v >> MPU6050_FSYNC_INT_EN_BIT) & 1);
        if (i2c_bypass)
            *i2c_bypass = !!((v >> MPU6050_I2C_BYPASS_EN_BIT) & 1);
    }

    static inline char mpu6050_smplrt_div(char smplrt_div)
    {
        return smplrt_div;
    }

    /* Interrupt enable configuration */
    typedef enum
    {
        MPU6050_INT_DISABLED = 0,
        MPU6050_INT_DATA_RDY_EN = 1 << 0,
        MPU6050_INT_FIFO_OFLOW_EN = 1 << 4,
        MPU6050_INT_I2C_MST_INT_EN = 1 << 3
    } mpu6050_int_en_t;

#define MPU6050_USERCTRL_FIFO_EN_BIT 6
#define MPU6050_USERCTRL_I2C_MST_EN_BIT 5
#define MPU6050_USERCTRL_I2C_IF_DIS_BIT 4
#define MPU6050_USERCTRL_FIFO_RESET_BIT 2
#define MPU6050_USERCTRL_I2C_MST_RESET_BIT 1

    /* USER_CTRL */
    typedef enum
    {
        MPU6050_USERCTRL_NONE = 0,
        MPU6050_USERCTRL_FIFO_EN = (1 << MPU6050_USERCTRL_FIFO_EN_BIT),
        MPU6050_USERCTRL_I2C_MST_EN = (1 << MPU6050_USERCTRL_I2C_MST_EN_BIT),
        MPU6050_USERCTRL_I2C_IF_DIS = (1 << MPU6050_USERCTRL_I2C_IF_DIS_BIT),
        MPU6050_USERCTRL_FIFO_RESET = (1 << MPU6050_USERCTRL_FIFO_RESET_BIT),
        MPU6050_USERCTRL_I2C_MST_RESET = (1 << MPU6050_USERCTRL_I2C_MST_RESET_BIT)
    } mpu6050_user_ctrl_en_t;

    typedef struct mpu6050_config_t
    {
        mpu6050_accel_scale_t accel_scale;
        mpu6050_gyro_scale_t gyro_scale;
        mpu6050_config_t dlpf_cfg;
        char sample_rate_div; /* Sample Rate = Gyro Output Rate / (1 + SMPLRT_DIV) */
        mpu6050_fifo_en_t fifo_en;
        mpu6050_int_pin_cfg_t int_pin_cfg;
        mpu6050_int_en_t int_enable;
        mpu6050_user_ctrl_en_t user_ctrl;
    } mpu6050_config_full_t;

    typedef enum
    {
        MPU6050_SIGNAL_PATH_RESET_NONE = 0,
        MPU6050_SIGNAL_PATH_RESET_TEMP_RST = (1 << 0),
        MPU6050_SIGNAL_PATH_RESET_ACCEL_RST = (1 << 1),
        MPU6050_SIGNAL_PATH_RESET_GYRO_RST = (1 << 2)
    } mpu6050_signal_path_reset_t;

#define MPU6050_SIGNAL_PATH_RESET_ALL (MPU6050_SIGNAL_PATH_RESET_TEMP_RST | MPU6050_SIGNAL_PATH_RESET_ACCEL_RST | MPU6050_SIGNAL_PATH_RESET_GYRO_RST)

    typedef enum
    {
        MPU6050_INT_STATUS_NONE = 0,
        MPU6050_INT_STATUS_DATA_RDY = 1 << 0,
        MPU6050_INT_STATUS_FIFO_OFLOW = 1 << 4,
        MPU6050_INT_STATUS_I2C_MST_INT = 1 << 3
    } mpu6050_int_status_t;

#define DEFAULT_MPU6050_CONFIG                               \
    {                                                        \
        MPU6050_ACCEL_SCALE_2G,                              \
        MPU6050_GYRO_SCALE_250,                              \
        MPU6050_DLPF_CF_184HZ,                               \
        mpu6050_smplrt_div(7),                               \
        MPU6050_FIFO_ENABLE_ALL,                             \
        mpu6050_int_pin_cfg_pack(                            \
            MPU6050_INT_LEVEL_ACTIVE_LOW,                    \
            MPU6050_INT_PUSH_PULL,                           \
            MPU6050_INT_PULSE,                               \
            MPU6050_INT_RD_CLEAR_BY_READING_INT_STATUS,      \
            0,                                               \
            0,                                               \
            0),                                              \
        MPU6050_INT_DATA_RDY_EN | MPU6050_INT_FIFO_OFLOW_EN, \
        MPU6050_USERCTRL_FIFO_EN}

    /* Plain return codes */
    typedef enum
    {
        MPU6050_OK = 0,
        MPU6050_ERR = -1,
        MPU6050_ERR_BAD_PARAM = -2,
        MPU6050_ERR_NOT_INITIALIZED = -3
    } mpu6050_status_t;

    /* Raw sensor sample container */
    typedef struct
    {
        short ax;
        short ay;
        short az;
        short temp;
        short gx;
        short gy;
        short gz;
    } mpu6050_raw_t;

    typedef struct
    {
        float ax;
        float ay;
        float az; /* in g */
        float gx;
        float gy;
        float gz;   /* in deg/sec */
        float temp; /* in deg C */
    } mpu6050_sample_float_t;

    /* Fixed-point converted sample */
    typedef struct
    {
        int ax_mg;
        int ay_mg;
        int az_mg; /* milli-g */
        int gx_mdps;
        int gy_mdps;
        int gz_mdps;    /* milli-deg/sec */
        int temp_mdegC; /* milli-deg C */
    } mpu6050_sample_fixed_t;

#ifdef __cplusplus
}
#endif

#endif /* MPU6050_H_ */
