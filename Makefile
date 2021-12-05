MAKE_DIR := /lib/modules/$(shell uname -r)/build

COMMON_CFLAGS := -Wall -Werror -Wextra

obj-m += mypantry.o

CFLAGS_mypantry.o += $(COMMON_CFLAGS)

KASAN_SANITIZE_mypantry.o := $(CONFIG_KASAN)
UBSAN_SANITIZE_mypantry.o := $(CONFIG_UBSAN)

all: kmod format_disk_as_pantryfs

format_disk_as_pantryfs: CC = gcc
format_disk_as_pantryfs: CFLAGS = -g $(COMMON_CFLAGS)

PHONY += kmod
kmod:
	$(MAKE) -C $(MAKE_DIR) M=$(PWD) modules

PHONY += clean
clean:
	$(MAKE) -C $(MAKE_DIR) M=$(PWD) clean
	rm -f format_disk_as_pantryfs
	git checkout -- ref/* # module clean deletes the modules in ref/

.PHONY: $(PHONY)
