#!/bin/bash
# MiniDOS 自定义内核配置脚本
# 目标: 极简、全静态、无模块、只保留本系统需要的功能
set -e
cd /root/build/ksrc/linux-6.12.115

echo "[1] 基础配置: 精简 + 全静态"
./scripts/config --file .config \
  --disable MODULES \
  --disable MODULE_SIG \
  --disable DEBUG_INFO \
  --disable DEBUG_KERNEL \
  --disable FTRACE \
  --disable KPROBES \
  --disable PROFILING \
  --disable BLK_DEV_DM \
  --disable MD \
  --disable SCSI \
  --disable ATA \
  --disable IDE \
  --disable CDROM \
  --disable USB \
  --disable SOUND \
  --disable WLAN \
  --disable WIRELESS \
  --disable BT \
  --disable HID \
  --enable INPUT \
  --enable INPUT_KEYBOARD \
  --enable INPUT_EVDEV \
  --enable INPUT_MOUSEDEV \
  --enable MOUSE_PS2 \
  --enable MOUSE_PS2_VMMOUSE \
  --enable KEYBOARD_ATKBD \
  --enable SERIO \
  --enable SERIO_I8042 \
  --enable SERIO_LIBPS2

echo "[2] 引导与内存"
./scripts/config --file .config \
  --enable BLK_DEV_INITRD=y \
  --enable RD_GZIP=y \
  --enable DEVTMPFS=y \
  --enable DEVTMPFS_MOUNT=y \
  --enable TMPFS=y \
  --enable PROC_FS=y \
  --enable SYSFS=y \
  --enable TTY=y \
  --enable VT=y \
  --enable VT_CONSOLE=y \
  --enable UNIX98_PTYS=y \
  --enable BINFMT_ELF=y \
  --enable BINFMT_SCRIPT=y \
  --enable BINFMT_MISC=y

echo "[3] 图形: DRM + simpledrm + bochs + fbcon"
./scripts/config --file .config \
  --enable DRM=y \
  --enable DRM_KMS_HELPER=y \
  --enable DRM_TTM=y \
  --enable DRM_TTM_HELPER=y \
  --enable DRM_SHMEM=y \
  --enable DRM_GEM_SHMEM_HELPER=y \
  --enable DRM_SIMPLEDRM=y \
  --enable DRM_BOCHS=y \
  --enable DRM_VIRTIO_GPU=y \
  --enable FB=y \
  --enable FB_CORE=y \
  --enable FB_EFI=y \
  --enable FB_VESA=y \
  --enable FRAMEBUFFER_CONSOLE=y \
  --enable FRAMEBUFFER_CONSOLE_DETECT_PRIMARY=y \
  --enable FB_SIMPLE=y \
  --enable FONT_8x16=y \
  --enable FONT_8x8=y \
  --enable FONT_SUPPORT=y

echo "[4] 网络: 有线网卡全静态"
./scripts/config --file .config \
  --enable NET=y \
  --enable INET=y \
  --enable NETDEVICES=y \
  --enable NET_CORE=y \
  --enable NET_PCI=y \
  --enable PCI=y \
  --enable E1000=y \
  --enable E1000E=y \
  --enable R8169=y \
  --enable IGB=y \
  --enable VIRTIO_NET=y \
  --enable VIRTIO_PCI=y \
  --enable VIRTIO_MENU=y \
  --enable PCI_MSI=y \
  --enable 8139TOO=y \
  --enable PCNET32=y \
  --enable TULIP=y \
  --enable NE2K_PCI=y \
  --enable PACKET=y \
  --enable DHCP=y \
  --enable IP_PNP=y \
  --enable TLS=y \
  --enable CRYPTO=y \
  --enable CRYPTO_AES=y \
  --enable CRYPTO_SHA256=y \
  --enable CRYPTO_SHA1=y

echo "[5] 文件系统"
./scripts/config --file .config \
  --enable EXT4_FS=y \
  --enable VFAT_FS=y \
  --enable ISO9660_FS=y \
  --enable SQUASHFS=y \
  --enable NLS=y \
  --enable NLS_DEFAULT="utf8" \
  --enable NLS_UTF8=y \
  --enable NLS_CODEPAGE_437=y

echo "[6] 时钟与杂项"
./scripts/config --file .config \
  --enable HPET_TIMER=y \
  --enable HIGH_RES_TIMERS=y \
  --enable RTC_CLASS=y \
  --enable RTC_HCTOSYS=y \
  --enable RANDOM_TRUST_CPU=y \
  --enable ACPI=y \
  --enable PCI_MMCONFIG=y \
  --enable X86_64=y \
  --enable EFI=y \
  --enable EFI_STUB=y

echo "[7] 关闭不需要的大块功能以减小体积"
./scripts/config --file .config \
  --disable SOUND \
  --disable MEDIA_SUPPORT \
  --disable DVB_CORE \
  --disable IIO \
  --disable STAGING \
  --disable INFINIBAND \
  --disable ISDN \
  --disable NET_VENDOR_MELLANOX \
  --disable NET_VENDOR_BROADCOM \
  --disable IPV6 \
  --disable NETFILTER \
  --disable MAC80211 \
  --disable CFG80211 \
  --disable CRYPTO_MANAGER_DISABLE_TESTS \
  --disable X86_MCE \
  --disable KEXEC \
  --disable CRASH_DUMP \
  --disable RELOCATABLE

echo "[8] 必要选项: 允许未知参数"
./scripts/config --file .config --enable EXPERT

echo "[9] 应用配置..."
make olddefconfig 2>&1 | tail -3

echo "[10] 配置摘要:"
echo "  静态编入(=y): $(grep -c '=y$' .config) 项"
echo "  模块(=m)    : $(grep -c '=m$' .config) 项"
echo "  内核大小预估: $(grep -c '=y$' .config) 项功能"
