DEVNAME := i2c_bmp280
ifneq ($(KERNELRELEASE),)
	obj-m += $(DEVNAME).o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	dtc -@ -I dts -O dtb -o $(DEVNAME)-overlay.dtbo device_tree.dts
endif

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(DEVNAME)-overlay.dtbo
