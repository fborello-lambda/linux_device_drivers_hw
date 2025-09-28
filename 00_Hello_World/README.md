<h1 align="center">
    Hello World
</h1>

Starting from a fresh Raspberry Pi OS installation, follow these steps:

### Install libraries

```sh
sudo apt update && sudo apt upgrade -y
sudo reboot
```

and then:

```sh
sudo apt install build-essential linux-headers-$(uname -r)
```

### How

The provided `Makefile` runs the `modules` target of the `Makefile` located at `/lib/modules/$(shell uname -r)/build`.

Create the `.ko` file:

```sh
make
```

Insert the module into the system:

```sh
sudo insmod hello.ko howmany=4 whom=kernel
```

To inspect the latest kernel messages — the ones written with the `printk()` function — use:

```sh
sudo dmesg | tail
```

Finally, remove the module with:

```sh
sudo rmmod hello
```

The kernel exit messages can be viewed by re-running the same command:

```sh
sudo dmesg | tail
```

The expected output should be similar to the following:

```text
[64123.519889] hello: loading out-of-tree module taints kernel.
[64123.526354] [0] Hello, "kernel"!
[64123.526383] [1] Hello, "kernel"!
[64123.526391] [2] Hello, "kernel"!
[64123.526397] [3] Hello, "kernel"!
[64123.526404] The calculated KERNEL_VERSION is: 396298 and the LINUX_VERSION_CODE is: 396313
[64123.526412] The process calling this module is: "insmod" (pid 51172)
[64220.033772] Goodbye, World!
```

## Extra Notes

- The module won’t compile without the `MODULE_LICENSE()` macro.
- The included headers are located at `/lib/modules/$(shell uname -r)/build/include/`.
  - For example, `linux/modules.h` is at `../build/include/linux/modules.h`.
- Kernel modules can be dynamically loaded and unloaded ("linked" and "unlinked") from the kernel. That’s why they are compiled as kernel objects (`.ko` files).
