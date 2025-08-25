import os
import time

# I2C address of BMP280
I2C_ADDR = "0x77"
I2C_BUS = "1"


def i2cget(register):
    # Use sudo i2cget -y 1 0x77 register and read hex value
    cmd = f"sudo i2cget -y {I2C_BUS} {I2C_ADDR} {register}"
    result = os.popen(cmd).read().strip()
    return int(result, 16)


def read_16bit_le(lsb_reg):
    # Read 16-bit little endian value (LSB then MSB)
    lsb = i2cget(lsb_reg)
    msb = i2cget(hex(int(lsb_reg, 16) + 1))
    print(
        f"Reading {lsb_reg} | {hex(int(lsb_reg, 16) + 1)}: LSB={lsb:02X}, MSB={msb:02X}"
    )
    value = (msb << 8) | lsb
    if value > 32767:  # signed conversion
        value -= 65536
    return value


# From BMP280 datasheet, compensates raw temperature value
def bmp280_compensate_T_int32(adc_T, dig_T1, dig_T2, dig_T3):
    var1 = ((((adc_T >> 3) - (dig_T1 << 1))) * dig_T2) >> 11
    var2 = (((((adc_T >> 4) - dig_T1) * ((adc_T >> 4) - dig_T1)) >> 12) * dig_T3) >> 14
    t_fine = var1 + var2
    T = (t_fine * 5 + 128) >> 8
    return T


def main():
    # Initialize BMP280 (reset and set config)
    # Reset:
    # sudo i2cset -y 1 0x77 0xE0 0xB6
    # Set filter
    # sudo i2cset -y 1 0x77 0xF5 0x90
    # Change Mode:
    # sudo i2cset -y 1 0x77 0xF4 0x43
    os.popen(f"sudo i2cset -y {I2C_BUS} {I2C_ADDR} 0xE0 0xB6")
    time.sleep(0.05)
    os.popen(f"sudo i2cset -y {I2C_BUS} {I2C_ADDR} 0xF5 0x90")
    time.sleep(0.05)
    os.popen(f"sudo i2cset -y {I2C_BUS} {I2C_ADDR} 0xF4 0x43")
    time.sleep(0.05)

    # Read calibration params dig_T1, dig_T2, dig_T3
    dig_T1 = read_16bit_le("0x88")
    dig_T2 = read_16bit_le("0x8A")
    dig_T3 = read_16bit_le("0x8C")
    print(f"dig_T1: {dig_T1}, dig_T2: {dig_T2}, dig_T3: {dig_T3}")
    time.sleep(0.05)

    # Read raw temperature registers FA, FB, FC
    A = i2cget("0xFA")
    B = i2cget("0xFB")
    C = i2cget("0xFC")
    print(f"0xFA: {A:02X}, 0xFB: {B:02X}, 0xFC: {C:02X}")

    adc_T = (A << 12) + (B << 4) + ((C & 0xF0) >> 4)
    print(f"Raw Temperature: {adc_T}")

    temp = bmp280_compensate_T_int32(adc_T, dig_T1, dig_T2, dig_T3)

    print(f"Temperature Float: {temp / 100:.2f} °C")


if __name__ == "__main__":
    main()
