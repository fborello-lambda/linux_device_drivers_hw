<h1 align="center">
    BBB and MPU6050
</h1>

### What

The idea of this project is to use the I2C bus of the BeagleBone Black (BBB) to read data from the MPU6050 sensor with the code of project `06`. The only difference is that the BBB uses a different device tree overlay (DTO).

### How

First of all, the device tree overlay has to be compiled. The source code is in the file `device_tree.dts`, it also has to be saved in a specific folder in the BBB. Moreover, the overlay has to be loaded during the boot process. To do so, the file `/boot/uEnv.txt` has to be modified to include the overlay in a `uboot_overlay_addrX` argument. This is the recommended way to load overlays in the BBB, as explained in the [Beagleboard:BeagleBoneBlack Debian - eLinux.org](https://elinux.org/Beagleboard:BeagleBoneBlack_Debian#U-Boot_Overlays) page. It's intended to be used with capes, but it works with custom overlays as well.

>[!NOTE]
> The overlay can also be loaded manually with the `fdt`
> command in the U-Boot console. Couldn't make it work, though.
> Seems a good option for testing, as it doesn't require a reboot.

First, compile the device tree overlay, save it in the `/lib/firmware` folder, and modify the `/boot/uEnv.txt` file to include the overlay. A script is provided to do all these steps automatically:

```sh
./init_mod.sh set_overlay
```

Then, reboot the BBB to load the overlay.

A quick way to check if the overlay is loaded is by running the following command:

```sh
sudo fdtdump /sys/firmware/fdt | grep "mpu6050"
```

`fdt` contains the device tree used by the kernel, and if the overlay is loaded, it should contain a node with the name `mpu6050`.

Finally, build and mount the module (Same as project `06`):

```sh
./init_mod.sh mount
```

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

To remove the module and delete the device file:

```sh
./init_mod.sh clean
```

>[!IMPORTANT]
> A Server written in C is provided. [See here](../server_for_mpu6050/c_server/README.md)
> A Python script is also provided to read and plot the data in real-time. [See here](../server_for_mpu6050/py_client/README.md)
> For the Python client the server IP must be changed to the BBB IP.

## Resources

* [Beagleboard:BeagleBoneBlack - eLinux.org](https://www.elinux.org/Beagleboard:BeagleBoneBlack)
* [how to flash debian image in the sd card to emmc in beaglebone black industrial - General Discussion - BeagleBoard](https://forum.beagleboard.org/t/how-to-flash-debian-image-in-the-sd-card-to-emmc-in-beaglebone-black-industrial/37114)
* [Beaglebone: Introduction to GPIOs - Using Device Tree Overlays under Linux 3.8+ - YouTube](https://www.youtube.com/watch?v=wui_wU1AeQc)
* [Connectors — BeagleBoard Documentation](https://docs.beagleboard.org/boards/beaglebone/black/ch07.html#)
* [Confused between Device Tree and Device Tree Overlay.What to use for the new Driver? - GoogleGroups - BeagleBoard](https://forum.beagleboard.org/t/confused-between-device-tree-and-device-tree-overlay-what-to-use-for-the-new-driver/25155)
* [Device Tree Overlays | Introduction to the BeagleBone Black Device Tree | Adafruit Learning System](https://learn.adafruit.com/introduction-to-the-beaglebone-black-device-tree/device-tree-overlays)
* [Loading a Device Tree Overlay (dtbo file) - General Discussion - BeagleBoard](https://forum.beagleboard.org/t/loading-a-device-tree-overlay-dtbo-file/38452)
* [U-Boot FDT Overlay FIT usage — Das U-Boot unknown version documentation](https://docs.u-boot.org/en/latest/usage/fit/overlay-fdt-boot.html)
* [The Beaglebone Black and Device Tree Overlays](https://www.ofitselfso.com/BeagleNotes/Beaglebone_Black_And_Device_Tree_Overlays.php)
* [Using Device Tree Overlays, example on BeagleBone Cape add-on boards - BeagleBoard](https://www.beagleboard.org/blog/2022-02-15-using-device-tree-overlays-example-on-beaglebone-cape-add-on-boards)
* [How to overlay device tree on beaglebone ai-64 board - General Discussion - BeagleBoard](https://forum.beagleboard.org/t/how-to-overlay-device-tree-on-beaglebone-ai-64-board/35134)
* [BeagleBone Black Enable SPIDEV - eLinux.org](https://elinux.org/BeagleBone_Black_Enable_SPIDEV)
* [Beagleboard:BeagleBoneBlack Debian - eLinux.org](https://elinux.org/Beagleboard:BeagleBoneBlack_Debian#U-Boot_Overlays)
* [fdt command — Das U-Boot unknown version documentation](https://docs.u-boot.org/en/latest/usage/cmd/fdt.html)
* [bb.org-overlays/src/arm/BBBLUE-MPU9250-00A0.dts at master · beagleboard/bb.org-overlays · GitHub](https://github.com/beagleboard/bb.org-overlays/blob/master/src/arm/BBBLUE-MPU9250-00A0.dts)
* [bb.org-overlays/src/arm at master · beagleboard/bb.org-overlays · GitHub](https://github.com/beagleboard/bb.org-overlays/tree/master/src/arm)
