# SPDX-License-Identifier: GPL-2.0

obj-m := multi_uio.o
KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm -f *.ko *.o *.mod* Module.symvers modules.order .*.cmd
