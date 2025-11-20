/** I2C2 low-level implementation for AM335x (BeagleBone),
 * following the 12-step initialization flow:
 *  1. Prescaler for I2C module (I2C_PSC)
 *  2. Configure the I2C clock for 100Kbps or 400Kbps (SCLL = x, SCLH = y)
 *  3. Enable the I2C module (I2C_CON:I2C_EN = 1)
 *  4. Configure I2C Control (I2C_CON)
 *  5. Enable the interrupts to be used (I2C_IRQENABLE_SET)
 *  6. Specify the address of the device to be used (I2C_SA = x)
 *  7. Specify the number of data bytes to be sent on the bus (I2C_CNT = x).
 *  8. Configure I2C_CON to start (STT) and stop (STP) the transfer
 *  9. Check I2C_IRQSTATUS to verify which enabled event triggered the interrupt
 *  10. Check I2C_IRQSTATUS_RAW to verify which event was generated whether enabled or disabled.
 *  11. Data reception:
 *   The I2C_IRQSTATUS:RRDY interrupt is generated when new data is available.
 *   The data is read from I2C_DATA.
 *  12. Data transmission:
 *   The I2C_IRQSTATUS:XRDY interrupt is generated when the data stored in I2C_DATA has been sent.
 * Using hardware interrupts (RRDY/XRDY/ARDY/NACK/AL) instead of polling.
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include "i2c2_ll.h"

static void __iomem *cm_per_base;
static void __iomem *i2c2_base;
static bool g_i2c2_ready;
static int g_i2c2_irq = -1;

static inline u32 i2c_r(u32 off) { return readl(i2c2_base + off); }
static inline void i2c_w(u32 off, u32 v) { writel(v, i2c2_base + off); }
static inline u32 cm_r(u32 off) { return readl(cm_per_base + off); }
static inline void cm_w(u32 off, u32 v) { writel(v, cm_per_base + off); }

/* Simple transaction context: only one active transaction at a time. */
enum i2c2_ll_phase
{
    I2C2_PHASE_IDLE = 0,
    I2C2_PHASE_WRITE, /* sending bytes */
    I2C2_PHASE_READ,  /* receiving bytes */
};

/**
 * Transaction state
 * Simple transaction context: only one active transaction at a time.
 * It holds the current phase, error status, buffer pointer, length, index,
 * and completion structure for synchronization.
 *
 * It has to be protected by g_xfer_lock mutex. To avoid concurrent access and race conditions.
 */
struct i2c2_ll_xfer
{
    enum i2c2_ll_phase phase;
    int error;              /* 0 or -Exxx */
    __u8 *buf;              /* pointer to data buffer */
    __u8 len;               /* total number of bytes to transfer */
    __u8 idx;               /* how many bytes processed */
    struct completion done; /* signaled on ARDY or error */
};

static DEFINE_MUTEX(g_xfer_lock);
static struct i2c2_ll_xfer g_xfer;

/** @brief IRQ handler for I2C2 interrupts.
 *
 * Handles RRDY, XRDY, ARDY, NACK, and AL interrupts.
 * Reads/writes data bytes and signals completion on transfer end or error.
 * @param irq IRQ number
 * @param dev_id Device ID (unused)
 *
 * @returns IRQ_HANDLED if handled, IRQ_NONE otherwise
 */
static irqreturn_t i2c2_ll_irq_handler(int irq, void *dev_id)
{
    u32 status, raw;

    if (!i2c2_base)
        return IRQ_NONE;

    raw = i2c_r(I2C_IRQSTATUS_RAW);
    status = i2c_r(I2C_IRQSTATUS);

    if (!raw)
        return IRQ_NONE;

    /* Error conditions first */
    if (status & (I2C_IRQ_NACK | I2C_IRQ_AL))
    {
        g_xfer.error = (status & I2C_IRQ_NACK) ? -ENXIO : -EAGAIN;
        g_xfer.phase = I2C2_PHASE_IDLE;
        i2c_w(I2C_IRQSTATUS, I2C_IRQ_NACK | I2C_IRQ_AL);
        complete(&g_xfer.done);
        return IRQ_HANDLED;
    }

    /* Transmission: XRDY => send next byte */
    if ((status & I2C_IRQ_XRDY) && g_xfer.phase == I2C2_PHASE_WRITE)
    {
        if (g_xfer.idx < g_xfer.len && g_xfer.buf)
        {
            i2c_w(I2C_DATA, g_xfer.buf[g_xfer.idx++]);
        }
        else
        {
            /* no more data, write dummy 0 */
            i2c_w(I2C_DATA, 0);
        }
        i2c_w(I2C_IRQSTATUS, I2C_IRQ_XRDY);
    }

    /* Reception: RRDY => read next byte */
    if ((status & I2C_IRQ_RRDY) && g_xfer.phase == I2C2_PHASE_READ)
    {
        if (g_xfer.idx < g_xfer.len && g_xfer.buf)
        {
            g_xfer.buf[g_xfer.idx++] = (u8)(i2c_r(I2C_DATA) & 0xFF);
        }
        else
        {
            (void)i2c_r(I2C_DATA); /* dummy read */
        }
        i2c_w(I2C_IRQSTATUS, I2C_IRQ_RRDY);
    }

    /* ARDY indicates transfer complete for current CNT */
    if (status & I2C_IRQ_ARDY)
    {
        i2c_w(I2C_IRQSTATUS, I2C_IRQ_ARDY);
        g_xfer.phase = I2C2_PHASE_IDLE;
        complete(&g_xfer.done);
    }

    return IRQ_HANDLED;
}

/** @brief Disable the I2C2 module clock/domain.
 *
 * Used during deinitialization.
 */
static void i2c2_disable_clk(void)
{
    if (!cm_per_base)
        return;
    cm_w(CM_PER_I2C2_CLKCTRL, cm_r(CM_PER_I2C2_CLKCTRL) & ~0x3U);
}

/** @brief Check whether the I2C2 module clock/domain is functional (not idle/gated).
 *
 * @returns true when it's safe to access I2C registers.
 */
static bool i2c2_clk_is_on(void)
{
    u32 v;

    if (!cm_per_base)
        return false;
    v = cm_r(CM_PER_I2C2_CLKCTRL);
    /* IDLEST bits are [17:16] == 0 when functional; MODULEMODE bits [1:0] non-zero when enabled */
    if (((v >> 16) & 0x3U) == 0 && (v & 0x3U) != 0)
        return true;
    return false;
}

/** @brief Ensure the I2C2 functional clock is enabled. Called before touching I2C regs.
 *
 * There seems to be an issue where the I2C2 clock can be disabled/gated unexpectedly,
 * leading to bus errors when accessing I2C registers. This function checks the clock
 * status and attempts to re-enable it if it's not functional.
 *
 * Not the most efficient way, but sufficient for our low-level driver.
 *
 * @returns 0 on success, -ETIMEDOUT on failure
 */
static int i2c2_ensure_clk(void)
{
    u32 v;
    int timeout = 200; /* up to ~2ms */

    if (!cm_per_base)
        return -ENODEV;

    v = cm_r(CM_PER_I2C2_CLKCTRL);
    if (((v >> 16) & 0x3U) == 0 && (v & 0x3U) != 0)
        return 0; /* already functional */

    /* try to enable module clock (set MODULEMODE = 0x2) */
    v &= ~0x3U;
    v |= 0x2U;
    cm_w(CM_PER_I2C2_CLKCTRL, v);

    while (timeout-- > 0)
    {
        v = cm_r(CM_PER_I2C2_CLKCTRL);
        if (((v >> 16) & 0x3U) == 0)
            return 0;
        udelay(10);
    }
    return -ETIMEDOUT;
}

/** @brief DEBUG: Dump key I2C2 registers for debugging
 *
 * @param tag Tag string to identify the dump context
 */
static void i2c2_ll_dump_state(const char *tag)
{
    if (!i2c2_base || !cm_per_base)
    {
        pr_err("i2c2_ll: dump_state(%s): bases NULL\n", tag);
        return;
    }

    /* Avoid reading I2C registers if the module clock is gated; doing so
     * on AM33xx can produce external aborts when the peripheral is inaccessible.
     */
    if (!i2c2_clk_is_on())
    {
        pr_warn("i2c2_ll: dump_state(%s): clock gated or module idle, skipping register read\n",
                tag);
        return;
    }

    pr_info("i2c2_ll: [%s] CON=0x%04x SA=0x%04x CNT=0x%04x DATA=0x%04x\n",
            tag,
            i2c_r(I2C_CON), i2c_r(I2C_SA), i2c_r(I2C_CNT), i2c_r(I2C_DATA));
    pr_info("i2c2_ll: [%s] IRQSTATUS=0x%04x RAW=0x%04x ENABLE=0x%04x\n",
            tag,
            i2c_r(I2C_IRQSTATUS), i2c_r(I2C_IRQSTATUS_RAW),
            i2c_r(I2C_IRQENABLE_SET));
}

/** @brief Initialize the low-level I2C2 interface with given bus frequency (kHz) and Linux IRQ
 *
 * @param bus_khz Bus frequency in kHz (e.g., 100 or 400)
 * @param irq Linux IRQ number for I2C2
 *
 * @returns 0 on success, <0 on error
 */
int i2c2_ll_init(unsigned int bus_khz, int irq)
{
    u32 v;
    int timeout;
    int ret;

    if (g_i2c2_ready)
        return 0;

    if (irq <= 0)
        return -EINVAL;
    g_i2c2_irq = irq;

    if (!cm_per_base)
    {
        cm_per_base = ioremap(AM33XX_CM_PER_BASE, 0x1000);
        if (!cm_per_base)
            return -ENOMEM;
    }
    if (!i2c2_base)
    {
        i2c2_base = ioremap(AM33XX_I2C2_BASE, AM33XX_I2C_MAP_SIZE);
        if (!i2c2_base)
        {
            iounmap(cm_per_base);
            cm_per_base = NULL;
            return -ENOMEM;
        }
    }

    if (!bus_khz)
        bus_khz = 100;

    pr_info("i2c2_ll: init (bus=%ukHz)\n", bus_khz);

    /* 1. Enable I2C2 module clock via CM_PER */
    v = cm_r(CM_PER_I2C2_CLKCTRL);
    v &= ~0x3U;
    v |= 0x2U; /* module enabled */
    cm_w(CM_PER_I2C2_CLKCTRL, v);

    timeout = 1000;
    while (timeout-- > 0)
    {
        v = cm_r(CM_PER_I2C2_CLKCTRL);
        if (((v >> 16) & 0x3) == 0)
            break; /* functional/idle */
        udelay(10);
    }
    if (timeout <= 0)
    {
        pr_err("i2c2_ll: clock enable timeout\n");
        return -ETIMEDOUT;
    }

    /* 2. Soft reset */
    i2c_w(I2C_SYSC, I2C_SYSC_SOFTRESET);
    timeout = 100;
    while (timeout-- > 0)
    {
        if (i2c_r(I2C_SYSS) & I2C_SYSS_RDONE)
            break;
        udelay(10);
    }
    if (timeout <= 0)
        pr_warn("i2c2_ll: reset timeout, continuing\n");

    /* 3. Disable module before programming */
    i2c_w(I2C_CON, 0);
    udelay(10);

    /* 4/5. Configure prescaler and SCLL/SCLH for 100kHz or 400kHz */
    if (bus_khz >= 400)
    {
        i2c_w(I2C_PSC, 3);
        i2c_w(I2C_SCLL, 10);
        i2c_w(I2C_SCLH, 12);
    }
    else
    {
        i2c_w(I2C_PSC, 23);
        i2c_w(I2C_SCLL, 53);
        i2c_w(I2C_SCLH, 55);
    }
    i2c_w(I2C_OA, 0x01);    /* own address (not used in master mode) */
    i2c_w(I2C_BUF, 0x0000); /* no FIFO, pure IRQ-based */

    /* 6/7/8. Enable interrupts and module */
    i2c_w(I2C_IRQSTATUS, 0xFFFF); /* clear all pending */
    i2c_w(I2C_IRQENABLE_SET,
          I2C_IRQ_XRDY | I2C_IRQ_RRDY | I2C_IRQ_ARDY |
              I2C_IRQ_NACK | I2C_IRQ_AL);

    i2c_w(I2C_CON, I2C_CON_EN);
    udelay(50);

    /* Initialize transfer context and register IRQ handler */
    init_completion(&g_xfer.done);
    g_xfer.phase = I2C2_PHASE_IDLE;
    g_xfer.error = 0;
    g_xfer.buf = NULL;
    g_xfer.len = 0;
    g_xfer.idx = 0;

    ret = request_irq(g_i2c2_irq, i2c2_ll_irq_handler,
                      IRQF_NO_THREAD, "i2c2_ll", NULL);
    if (ret)
    {
        pr_err("i2c2_ll: request_irq failed: %d\n", ret);
        i2c_w(I2C_CON, 0);
        i2c2_disable_clk();
        iounmap(i2c2_base);
        i2c2_base = NULL;
        iounmap(cm_per_base);
        cm_per_base = NULL;
        return ret;
    }

    g_i2c2_ready = true;
    return 0;
}

/** @brief Deinitialize the low-level I2C2 interface */
void i2c2_ll_deinit(void)
{
    if (!g_i2c2_ready)
        return;

    if (g_i2c2_irq > 0)
        free_irq(g_i2c2_irq, NULL);

    if (i2c2_base)
        i2c_w(I2C_CON, 0);
    i2c2_disable_clk();
    if (i2c2_base)
        iounmap(i2c2_base);
    if (cm_per_base)
        iounmap(cm_per_base);
    i2c2_base = NULL;
    cm_per_base = NULL;
    g_i2c2_irq = -1;
    g_i2c2_ready = false;
}

/** @brief Check whether the I2C2 low-level interface is initialized
 *
 * @returns true if initialized, false otherwise
 */
bool i2c2_ll_is_initialized(void)
{
    return g_i2c2_ready;
}

/** @brief INTERNAL: Start a simple write transfer (len bytes from buf)
 *
 * @param sa Slave address
 * @param buf Buffer containing data to write
 * @param len Number of bytes to write
 *
 * @returns 0 on success, <0 on error
 */
static int i2c2_start_write(__u8 sa, __u8 *buf, __u8 len)
{
    long timeout;
    int rc;

    /* Ensure controller clock is enabled before touching regs */
    rc = i2c2_ensure_clk();
    if (rc)
    {
        pr_err("i2c2_ll: write: cannot enable clock: %d\n", rc);
        return rc;
    }
    reinit_completion(&g_xfer.done);
    g_xfer.phase = I2C2_PHASE_WRITE;
    g_xfer.error = 0;
    g_xfer.buf = buf;
    g_xfer.len = len;
    g_xfer.idx = 0;

    i2c_w(I2C_IRQSTATUS, 0xFFFF);
    i2c_w(I2C_SA, sa);
    i2c_w(I2C_CNT, len);
    i2c_w(I2C_CON, I2C_CON_EN | I2C_CON_MST | I2C_CON_TRX |
                       I2C_CON_STT | I2C_CON_STP);

    // https://elixir.bootlin.com/linux/v6.12/source/kernel/sched/completion.c#L152
    // Same as return wait_for_common(x, timeout, TASK_UNINTERRUPTIBLE);
    // Setting it as TASK_UNINTERRUPTIBLE when writing data.
    timeout = wait_for_completion_timeout(&g_xfer.done, msecs_to_jiffies(100));
    if (!timeout)
    {
        pr_err("i2c2_ll: write timeout (sa=0x%02x, len=%u)\n", sa, len);
        i2c2_ll_dump_state("write_timeout");
        /* Clear any sticky IRQ status to avoid poisoning next transfer */
        i2c_w(I2C_IRQSTATUS, 0xFFFF);
        return -ETIMEDOUT;
    }
    return g_xfer.error;
}

/** @brief INTERNAL: Start a simple read transfer (len bytes into buf)
 *
 * @param sa Slave address
 * @param buf Buffer to store read data
 * @param len Number of bytes to read
 *
 * @returns 0 on success, <0 on error
 */
static int i2c2_start_read(__u8 sa, __u8 *buf, __u8 len)
{
    long timeout;
    int rc;

    /* Ensure controller clock is enabled before touching regs */
    rc = i2c2_ensure_clk();
    if (rc)
    {
        pr_err("i2c2_ll: read: cannot enable clock: %d\n", rc);
        return rc;
    }
    reinit_completion(&g_xfer.done);
    g_xfer.phase = I2C2_PHASE_READ;
    g_xfer.error = 0;
    g_xfer.buf = buf;
    g_xfer.len = len;
    g_xfer.idx = 0;

    i2c_w(I2C_IRQSTATUS, 0xFFFF);
    i2c_w(I2C_SA, sa);
    i2c_w(I2C_CNT, len);
    i2c_w(I2C_CON, I2C_CON_EN | I2C_CON_MST |
                       I2C_CON_STT | I2C_CON_STP);

    // https://elixir.bootlin.com/linux/v6.12/source/kernel/sched/completion.c#L152
    // Same as return wait_for_common(x, timeout, TASK_UNINTERRUPTIBLE);
    // Setting it as TASK_UNINTERRUPTIBLE when writing data.
    timeout = wait_for_completion_timeout(&g_xfer.done, msecs_to_jiffies(100));
    if (!timeout)
    {
        pr_err("i2c2_ll: read timeout (sa=0x%02x, len=%u)\n", sa, len);
        i2c2_ll_dump_state("read_timeout");
        i2c_w(I2C_IRQSTATUS, 0xFFFF);
        return -ETIMEDOUT;
    }
    return (g_xfer.error) ? g_xfer.error : 0;
}

/** @brief Write a single register: write reg address and value
 *
 * @param sa Slave address
 * @param reg Register address
 * @param val Value to write
 *
 * @returns 0 on success, <0 on error
 */
int i2c2_ll_write_byte(__u8 sa, __u8 reg, __u8 val)
{
    int ret;
    __u8 buf[2];

    if (!i2c2_ll_is_initialized())
        return -ENODEV;

    mutex_lock(&g_xfer_lock);
    buf[0] = reg;
    buf[1] = val;
    ret = i2c2_start_write(sa, buf, 2);
    mutex_unlock(&g_xfer_lock);
    return ret;
}

/** @brief Read a single register: write reg address, then read one byte
 *
 * @param sa Slave address
 * @param reg Register address
 *
 * @returns 0..255 on success, <0 on error
 */
int i2c2_ll_read_byte(__u8 sa, __u8 reg)
{
    int ret;
    __u8 buf;

    if (!i2c2_ll_is_initialized())
        return -ENODEV;

    mutex_lock(&g_xfer_lock);

    buf = reg;
    ret = i2c2_start_write(sa, &buf, 1);
    if (ret)
        goto out;

    ret = i2c2_start_read(sa, &buf, 1);
    if (ret == 0)
        ret = buf; /* return the byte as int */
    else
        pr_err("i2c2_ll: read_byte failed sa=0x%02x reg=0x%02x ret=%d\n",
               sa, reg, ret);

out:
    mutex_unlock(&g_xfer_lock);
    return ret;
}

/** @brief Read a block of registers: write reg address, then read len bytes
 *
 * @param sa Slave address
 * @param reg Starting register address
 * @param len Number of bytes to read
 * @param buf Buffer to store read data
 *
 * @returns 0 on success, <0 on error
 */
int i2c2_ll_read_block(__u8 sa, __u8 reg, __u8 len, __u8 *buf)
{
    int ret;

    if (!i2c2_ll_is_initialized())
        return -ENODEV;
    if (!buf || !len)
        return -EINVAL;

    mutex_lock(&g_xfer_lock);

    ret = i2c2_start_write(sa, &reg, 1);
    if (ret)
        goto out;

    ret = i2c2_start_read(sa, buf, len);

out:
    mutex_unlock(&g_xfer_lock);
    return ret;
}
