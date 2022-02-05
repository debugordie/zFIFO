# To cross-compile zfifo :
#   make ARCH={arm,arm64} KERNEL_SRC_DIR=/path/to/your/kernel/source/tree

# CROSS_COMPILE is compiler prefix, aarch64-linux-gnu- is default for ARM64.
# CC_SUFFIX is compiler suffix: ex) CC_SUFFIX="-9" for aarch64-linux-gnu-gcc-9

ARCH  ?= $(shell uname -m | sed -e s/arm.*/arm/ -e s/aarch64.*/arm64/)
KERNEL_SRC_DIR  ?= /lib/modules/$(shell uname -r)/build

CC_SUFFIX ?= ""

ifeq ($(ARCH), arm)
 ifneq ($(HOST_ARCH), arm)
   CROSS_COMPILE  ?= arm-linux-gnueabihf-
 endif
endif 

ifeq ($(ARCH), arm64)
 ifneq ($(HOST_ARCH), arm64)
   CROSS_COMPILE  ?= aarch64-linux-gnu-
 endif
endif

obj-m := zfifo.o

all: zfifo.ko libzfifo.so.1 libzfifo-test

libzfifo-test: libzfifo.c libzfifo-test.c
	$(CROSS_COMPILE)gcc$(CC_SUFFIX) libzfifo-test.c libzfifo.c -fopenmp -Wall -olibzfifo-test

libzfifo.so.1: libzfifo.c zfifo.h
	$(CROSS_COMPILE)gcc$(CC_SUFFIX) -shared -Wl,-soname,libzfifo.so.1 -o libzfifo.so.1 libzfifo.c

zfifo.ko: zfifo.c
	make -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules

clean:
	make -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean
	rm -f libzfifo.so.1 libzfifo-test *~
