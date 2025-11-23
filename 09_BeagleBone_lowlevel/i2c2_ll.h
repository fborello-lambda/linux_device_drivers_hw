#ifndef I2C2_LL_H
#define I2C2_LL_H

#include <linux/types.h>

/*
 * The registers are treated as constants, and set by #defines below.
 *
 * The i2c2_ll_init() function could ask for the registers base addresses,
 * and they could be read from the device tree in a real driver.
 *
 * NOT DONE in this example. Just to keep it simple.
 */
/* AM335x I2C2 base and PRCM (CM_PER) */
#define AM33XX_CM_PER_BASE 0x44E00000UL
#define CM_PER_I2C2_CLKCTRL 0x44 /* offset from CM_PER base */

#define AM33XX_I2C2_BASE 0x4819C000UL
#define AM33XX_I2C_MAP_SIZE 0x1000

/* I2C register offsets (relative to AM33XX_I2C2_BASE) */
#define I2C_SYSC 0x10
#define I2C_IRQSTATUS_RAW 0x24
#define I2C_IRQSTATUS 0x28
#define I2C_IRQENABLE_SET 0x2C
#define I2C_IRQENABLE_CLR 0x30 // Not used
#define I2C_SYSS 0x90
#define I2C_BUF 0x94
#define I2C_CNT 0x98
#define I2C_DATA 0x9C
#define I2C_CON 0xA4
#define I2C_OA 0xA8
#define I2C_SA 0xAC
#define I2C_PSC 0xB0
#define I2C_SCLL 0xB4
#define I2C_SCLH 0xB8
#define I2C_BUFSTAT 0xBC // Not used

/* I2C_CON bits */
#define I2C_CON_EN (1U << 15)
#define I2C_CON_OPMODE_HS (1U << 12) // Not used
#define I2C_CON_STB (1U << 11)       // Not used
#define I2C_CON_MST (1U << 10)
#define I2C_CON_TRX (1U << 9)
#define I2C_CON_XA (1U << 8) // Not used
#define I2C_CON_STP (1U << 1)
#define I2C_CON_STT (1U << 0)

/* IRQSTATUS bits */
#define I2C_IRQ_AL (1U << 0)
#define I2C_IRQ_NACK (1U << 1)
#define I2C_IRQ_ARDY (1U << 2)
#define I2C_IRQ_RRDY (1U << 3)
#define I2C_IRQ_XRDY (1U << 4)
#define I2C_IRQ_BB (1U << 12)
#define I2C_IRQ_AAS (1U << 9) // Not used

/* SYSC bits */
#define I2C_SYSC_SOFTRESET (1U << 1)

/* SYSS bits */
#define I2C_SYSS_RDONE (1U << 0)

int i2c2_ll_init(unsigned int bus_khz, int irq);
void i2c2_ll_deinit(void);
bool i2c2_ll_is_initialized(void);

int i2c2_ll_write_byte(__u8 sa, __u8 reg, __u8 val);
int i2c2_ll_read_byte(__u8 sa, __u8 reg);
int i2c2_ll_read_block(__u8 sa, __u8 reg, __u8 len, __u8 *buf);

#endif /* I2C2_LL_H */
