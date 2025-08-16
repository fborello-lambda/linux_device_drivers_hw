ifneq ($(KERNELRELEASE),)
	obj-m += gpio_7seg_dtree.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	dtc -@ -I dts -O dtb -o gpio_7seg-overlay.dtbo device_tree.dts
endif

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f gpio_7seg-overlay.dtbo
