# TianjiLKM v4.1 - pearl seven-layer anti-brick (reviewed)
obj-m += tianji_lkm.o
KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
LLVM ?= 1
PWD := $(shell pwd)
ccflags-y += -Wall -Wno-declaration-after-statement

.PHONY: all modules clean
all modules:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) LLVM=$(LLVM) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
