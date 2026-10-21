obj-m += dmabuf_probe.o
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
	$(CC) -O2 -Wall -Wextra -Werror -o probe_user probe_user.c
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) probe_user
