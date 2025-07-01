SDK_DIR = /milkv/duos/duo-buildroot-sdk-v2
KERN_DIR = $(SDK_DIR)/linux_5.10/build/sg2000_milkv_duos_musl_riscv64_sd

all:
	make -C $(KERN_DIR) M=$(PWD) modules

clean:
	make -C $(KERN_DIR) M=$(PWD) modules clean
	rm -rf modules.order

obj-m += sg2000_video_debug.o