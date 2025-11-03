KOBJECT := mpu6050

# Build a single module 'mpu6050.ko' from multiple .o files:
# adjust the object names to match your source files
mpu6050-objs := mpu6050_core.o mpu6050_kdd_primitives.o

ifneq ($(KERNELRELEASE),)
    obj-m += $(KOBJECT).o
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

# Search for the first dt-bindings include directory matching the current kernel version
UNAME := $(shell uname -r)
UNAME_BASE := $(shell echo $(UNAME) | cut -d+ -f1)

# Find dt-bindings include dir: prefer exact uname, then base version, then any found
DTC_INC := $(shell \
    find /usr /lib /boot /usr/src -type d -path "*/include/dt-bindings" 2>/dev/null | \
    grep -F "$(UNAME)" | head -n1 | xargs dirname 2>/dev/null || \
    find /usr /lib /boot /usr/src -type d -path "*/include/dt-bindings" 2>/dev/null | \
    grep -F "$(UNAME_BASE)" | head -n1 | xargs dirname 2>/dev/null || \
    find /usr /lib /boot /usr/src -type d -path "*/include/dt-bindings" 2>/dev/null | \
    head -n1 | xargs dirname 2>/dev/null || \
    true)

DTS      := device_tree.dts
# Preprocessed DTS file (if needed)
# In case of includes, we need to preprocess the DTS file before passing it to dtc:
# https://stackoverflow.com/questions/50658326/device-tree-compiler-not-recognizes-c-syntax-for-include-files
PP_DTS   := $(DTS:.dts=.pp.dts)
DTBO     := $(KOBJECT)-overlay.dtbo

# Detect if preprocessing is needed (presence of include directives)
NEED_CPP := $(shell grep -E '^[[:space:]]*([#/]|/include/)' $(DTS) | grep -q 'include' && echo 1 || echo 0)

CPP      ?= cpp
CPPFLAGS := -nostdinc -undef -x assembler-with-cpp
ifneq ($(DTC_INC),)
CPPFLAGS += -I $(DTC_INC)
DTC_I_FLAG = -i $(DTC_INC)
endif

default: modules overlay

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

# Preprocess only if NEED_CPP=1 and we have an include dir
$(PP_DTS): $(DTS)
ifneq ($(NEED_CPP),0)
	@if [ -z "$(DTC_INC)" ]; then echo "No dt-bindings include dir found for preprocessing"; exit 1; fi
	@echo "CPP  $(DTS) -> $(PP_DTS)"
	$(CPP) $(CPPFLAGS) $< > $@
else
	@cp $< $@
endif

overlay: $(PP_DTS)
	@echo "DTC  $(DTBO) (include dir: $(if $(DTC_INC),$(DTC_INC),none), cpp: $(NEED_CPP))"
	dtc -@ -I dts -O dtb -o $(DTBO) $(DTC_I_FLAG) $(PP_DTS)

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(DTBO) $(PP_DTS)

.PHONY: default modules overlay clean
endif
