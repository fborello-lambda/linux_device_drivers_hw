<h1 align="center">
    MPU6050 Driver as Misc Platform Driver with Device Tree
</h1>

### What

This example demonstrates how to create a simple Linux kernel driver for the MPU6050 sensor using the **misc framework** and **device tree**. The driver reads data from the MPU6050 via I2C and exposes it through a character device interface.

The driver uses interrupts to handle data ready signals from the MPU6050, ensuring efficient data retrieval without busy-waiting.
It also creates a device file `/dev/mpu6050` that can be used to read sensor data.

The device tree is used to describe the hardware configuration, making the driver more portable across different platforms.

A file named `mpu6050_core.c` contains the core functionality of the MPU6050 driver, while `mpu6050_kdd_primitives.c` includes basic I2C read/write functions. A header file `mpu6050_lib.h` defines the necessary constants and data structures, it can be used in user-space applications.

>[!NOTE]
> This is a simple example for educational purposes.
> Maybe IOCTLs could be added to configure the sensor settings from user space and read the data without the need of parsing the string output. Or even better, using the `hwmon` subsystem to expose sensor data in a standardized way.

A thing to note is that the driver consists of multiple source files. To compile this, some extra steps are needed, as shown in the `Makefile`:

```Makefile
KOBJECT := mpu6050

# Build a single module 'mpu6050.ko' from multiple .o files:
# adjust the object names to match your source files
mpu6050-objs := mpu6050_core.o mpu6050_kdd_primitives.o
```

The KOBJECT variable defines the name of the module, while the `mpu6050-objs` variable lists all the object files that will be linked together to create the final kernel module.

### How

In order to connect the MPU6050 to your board, you need to connect the following pins(On a Raspberry Pi400):

- VCC to 3.3V
- GND to GND
- SDA to I2C1 Data Line (GPIO 2)
- SCL to I2C1 Clock Line (GPIO 3)
- INT to GPIO pin 24
- IRQ_Button with pull-up resistor to GPIO pin 23 (optional)

Make sure that the I2C interface is enabled on your Raspberry Pi. You can do this using `raspi-config`.

To check if the MPU6050 is connected and recognized by the I2C bus, you can use the command:

```sh
i2cdetect -y 1
```

The output should show the address `0x68`.

Simmilarly to the previous examples, to compile and load the module, you can use the provided script:

```sh
./init_mod.sh mount
```

This compiles and loads the module, creating the device file `/dev/mpu6050`.

To read data from the sensor, you can use:

```sh
cat /dev/mpu6050
```

The output will be a string with the sensor readings in the following format:

```txt
IRQ count: 0
 0.195, 0.023, 0.906, [g]
-5.844,-0.473, 0.495, [dps]
26.101, [°C]
```

If the IRQ_Button is connected and pressed, it will increment the IRQ count.

To remove the module and delete the device file:

```sh
./init_mod.sh clean
```

>[!IMPORTANT]
> A Server written in C is provided. [See here](../server_for_mpu6050/c_server/README.md)
> A Python script is also provided to read and plot the data in real-time. [See here](../server_for_mpu6050/py_client/README.md)

## Resources

- [MPU-6000-Datasheet1.pdf](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Datasheet1.pdf)
- [RS-MPU-6000A-00 - MPU-6000-Register-Map1.pdf](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf)
