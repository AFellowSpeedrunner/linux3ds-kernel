#!/bin/sh

TOOLCHAIN=/opt/armv6-eabihf--glibc--bleeding-edge-2020.02-2/bin/arm-buildroot-linux-gnueabihf-

cp arch/arm/configs/nintendo3ds_defconfig .config

make ARCH=arm CROSS_COMPILE=$TOOLCHAIN -j4
make ARCH=arm CROSS_COMPILE=$TOOLCHAIN nintendo3ds_ctr.dtb

echo "Output file: ./arch/arm/boot/zImage"
echo "Output DTB: ./arch/arm/boot/dts/nintendo3ds_ctr.dtb"
