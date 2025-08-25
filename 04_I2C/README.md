<h1 align="center">
    I2C
</h1>

First of all, the I2C Kernel Module has to be activated/loaded. The easiest way to do this is by running an interactive menu with `sudo raspi-config`, then selecting (3) **Interface Options** and finally enabling (I5) **I2C**. A prompt will appear asking for confirmation.

>[!NOTE]
> The package `i2c-tools` is extremely useful to debug devices and test if they are functioning correctly without hassle.  
> It should already be installed on the Raspberry Pi, but if not, the following command can be used to install it:  
> `sudo apt install i2c-tools`

Now, if an I2C device is connected to GPIOs 2 and 3 (which correspond to I2C bus 1), the device's address can be retrieved by probing the whole bus with `i2cdetect -y 1`. The output should look like the following:

```sh
$ i2cdetect -y 1
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- -- -- -- 
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
60: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
70: -- -- -- -- -- -- -- 77    
```

In this example, the device is connected to I2C bus 1 and the address is `0x77`.

### BMP280

The simplest I2C device may be the [BMP280](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf) sensor (check the wiring on page 37, section 6.3). It's well documented and there are a lot of tutorials online. Moreover, the configuration is simple and it has only a few registers. The address is configurable (`0x76` or `0x77`). In this case, we will continue with `0x77`.

After checking that the device is connected and the OS recognizes it by probing with `i2cdetect`, we can read and write to the device with `i2cget` and `i2cset`.

For example, if we read register `0xD0`, we will get the ID:

```sh
$ i2cget -y 1 0x77 0xD0
0x58
```

The sensor starts in **sleep** mode. It has to be changed to **forced** or **normal** mode to perform a measurement cycle. The normal mode is the easiest to use, because the sensor will be measuring continuously and the data can be read whenever needed.

In section 3.6.3 (Normal Mode), it says:

> After setting the mode, measurement, and filter options, the last measurement results can be obtained from the data registers without the need for further write accesses.

The `Memory Map` is described in section 4. The `config` (0xF5) register controls timings, and the `ctrl_meas` (0xF4) register controls the power mode as well as the oversampling.

A simple configuration can be loaded to the device with `i2cset`:

```sh
i2cset -y 1 0x77 0xE0 0xB6 # RESET
i2cset -y 1 0x77 0xF5 0x90
i2cset -y 1 0x77 0xF4 0x43
```

Then the data registers can be read to get the raw values. The compensation formula (section 3.11.3) is used to calculate the actual value.

A Python script is included. It configures the sensor, reads the calibration parameters, then reads the raw temperature value and applies the compensation formula to print the sensed temperature:

```sh
python bmp280.py
```

Ideally, the data has to be read as a stream/block or in burst mode according to the datasheet.

### What

The configuration values are hardcoded, there should be a way to configure the device via a structure or something similar. There are some `typedef` structures defined to hold the raw and compensated data. And the I2C client is defined as a global static variable. Ideally, it should be part of a structure that holds all the device-specific data.

By using the misc device abstraction and linking the `i2c_driver` to the `misc_device` struct, a simple I2C driver can be created, the device file is automatically generated and linked.

The `linux/i2c.h` library contains a function that reads a block out of an I2C device: `i2c_smbus_read_i2c_block_data`. It's useful to perform the read all at once.

### How

The easiest way to use the driver is to run the script:

```sh
./init_mod.sh mount
```

To read temperature and pressure from the device, read directly from the device file:

```sh
cat /dev/i2c_bmp280
```

The output should be something like the following. Pressure and temperature varies by blowing directly to the sensor:

```sh
$ cat /dev/i2c_bmp280
Temp: 21.95 °C, Press: 1017.48 hPa
```

To remove the module and delete the device file (this also resets the sensor):

```sh
./init_mod.sh clean
```

## Resources

* [bst-bmp280-ds001.pdf](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf)
* Example module written in Python:
  * [uPySensors/bmx280.py at master · lemariva/uPySensors](https://github.com/lemariva/uPySensors/blob/master/bmx280.py)
* [Let's code a Linux Driver - 7: BMP280 Driver (I2C Temperature Sensor) - YouTube](https://www.youtube.com/watch?v=j-zo1QOBUZg&list=PLCGpd0Do5-I1c2Qf_J0zJcfhFYEH0x3im)
* [Let's code a Linux Driver - 22: Device Tree driver for an I2C Device - YouTube](https://www.youtube.com/watch?v=GQ1XwFWA2Nw&list=PLCGpd0Do5-I3b5TtyqeF1UdyD4C-S-dMa)
* [Let's code a Linux Driver - 27: Misc device - The easy way to use device files in a LKM - YouTube](https://www.youtube.com/watch?v=kX7Mqw_e3JA&list=PLCGpd0Do5-I3b5TtyqeF1UdyD4C-S-dMa)
* [Writing a Simple misc Character Device Driver | Linux Kernel Programming Part 2 - Char Device Drivers and Kernel Synchronization](https://subscription.packtpub.com/book/cloud-and-networking/9781801079518/2/ch02lvl1sec07/writing-the-misc-driver-code-part-1)

## Extra Notes

* `i2c-tools`
  * `i2cset` – Write to an I2C device
  * `i2cget` – Read from an I2C device
  * `i2cdetect` – `i2cdetect -y 1` probes I2C bus 1
* [RPi.bme280 · PyPI](https://pypi.org/project/RPi.bme280/)
  * The module provides some example code. It's an easy way to make sure the Kernel Module and/or the Python script is working as expected, or at least behaving the same way as the PyPI module.
