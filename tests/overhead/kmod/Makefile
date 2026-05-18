ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
obj-m  := kmod_rdtsc_rdtscp.o

else
# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

endif
