obj-m += rtl8811au.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Add -Wall to the compiler flags
EXTRA_CFLAGS += -Wall

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm -rf *.o *.ko *.mod.o *.symvers .tmp_versions