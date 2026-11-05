#!/bin/bash
set -e
B=/root/build
DL=$B/dl; IR=$B/initramfs; ISO=$B/iso
KVER=6.12.115-custom

cd $B/src
echo "[1/6] 编译游戏 (静态链接)..."
gcc -static -O2 -o snake snake.c
gcc -static -O2 -o t2048 t2048.c

echo "[1.5/6] 编译智能体创作的软件 (创作管线)..."
mkdir -p $IR/bin
for app in $B/src/apps/*.c; do
  [ -e "$app" ] || continue
  name=$(basename "$app" .c)
  if gcc -static -O2 -o app-$name "$app" 2>/tmp/compile_err; then
    echo "  装入: 应用-$name"
  else
    echo "  编译失败: $name (已通知智能体)"
  fi
done

echo "[2/6] 组装 initramfs..."
rm -rf $IR; mkdir -p $IR/{bin,dev,proc,sys,tmp,root,etc}
cp $DL/busybox $IR/bin/busybox
cp $B/src/init $IR/init; chmod +x $IR/init
cp $B/src/snake $IR/bin/snake
cp $B/src/t2048 $IR/bin/t2048
cp $B/src/desktop $IR/bin/desktop
cp $B/src/agent $IR/bin/agent
# 智能体创作的软件 (创作管线产物)
for appbin in $B/app-*; do
  [ -e "$appbin" ] || continue
  cp "$appbin" $IR/bin/$(basename "$appbin" | sed 's/^app-/应用-/')
done
mkdir -p $IR/mem/chunks
cp -r $B/src/seedmem/chunks/* $IR/mem/chunks/ 2>/dev/null || true
cp $B/src/seedmem/index.txt $IR/mem/ 2>/dev/null || true
mkdir -p $IR/usr/share/udhcpc
cp $B/src/udhcpc.script $IR/usr/share/udhcpc/default.script
chmod +x $IR/usr/share/udhcpc/default.script

echo "[2.5/6] 打包完整版 wget (支持现代 TLS/ECDSA 证书)..."
mkdir -p $IR/lib/x86_64-linux-gnu $IR/lib64
cp /usr/bin/wget $IR/bin/wget2
for lib in $(ldd /usr/bin/wget | awk '{print $3}' | grep -v '^$'); do
  cp -L "$lib" $IR/lib/x86_64-linux-gnu/ 2>/dev/null
done
cp -L /lib64/ld-linux-x86-64.so.2 $IR/lib64/ 2>/dev/null
# NSS 库 (域名解析需要)
for lib in /lib/x86_64-linux-gnu/libnss_dns* /lib/x86_64-linux-gnu/libnss_files* /usr/lib/x86_64-linux-gnu/libnss_dns* /usr/lib/x86_64-linux-gnu/libnss_files*; do
  cp -L $lib $IR/lib/x86_64-linux-gnu/ 2>/dev/null
done
# CA 根证书 (HTTPS 验证) — wget/OpenSSL 默认查找 /usr/lib/ssl/certs
mkdir -p $IR/etc/ssl/certs $IR/usr/lib/ssl
cp /etc/ssl/certs/ca-certificates.crt $IR/etc/ssl/certs/ 2>/dev/null || true
cp /etc/ssl/certs/ca-certificates.crt $IR/etc/ssl/cert.pem 2>/dev/null || true
ln -sf /etc/ssl/certs $IR/usr/lib/ssl/certs
ln -sf /etc/ssl/cert.pem $IR/usr/lib/ssl/cert.pem

echo "[3/6] 自定义内核: 无模块, 跳过模块打包"

echo "[4/6] 打包 initramfs..."
cd $IR && find . | cpio -o -H newc 2>/dev/null | gzip -6 > $B/initrd.gz
du -sh $B/initrd.gz

echo "[5/6] 生成 GRUB 配置并打包 ISO..."
cp -f $B/vmlinuz-custom $B/vmlinuz
mkdir -p $ISO/boot/grub
cat > $ISO/boot/grub/grub.cfg <<'EOF'
set timeout=0
set default=0
menuentry "MiniDOS 1.0" {
    linux /boot/vmlinuz quiet console=tty0
    initrd /boot/initrd.gz
}
menuentry "MiniDOS 1.0 智能体模式" {
    linux /boot/vmlinuz quiet console=tty0 agent=on
    initrd /boot/initrd.gz
}
menuentry "MiniDOS 1.0 图形桌面" {
    linux /boot/vmlinuz quiet console=tty0 mini=gui
    initrd /boot/initrd.gz
}
menuentry "MiniDOS 1.0 (显示内核日志)" {
    linux /boot/vmlinuz console=tty0
    initrd /boot/initrd.gz
}
EOF
mkdir -p $ISO/boot
cp $B/vmlinuz $ISO/boot/vmlinuz
cp $B/initrd.gz $ISO/boot/initrd.gz
cd $B && rm -f minidos.iso
grub-mkrescue -o $B/minidos.iso $ISO --compress=no 2>/dev/null || grub-mkrescue -o $B/minidos.iso $ISO

echo "[6/6] 完成!"
ls -la $B/minidos.iso
