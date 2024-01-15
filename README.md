Linux 3DS kernel
================


In order to build the kernel, download [this toolchain](https://toolchains.bootlin.com/downloads/releases/toolchains/armv6-eabihf/tarballs/armv6-eabihf--glibc--bleeding-edge-2020.02-2.tar.bz2) and extract it to your /opt directory. 

Then, place your rootfs.cpio.gz image you got from the Buildroot3DS build process and place it in this directory.

After that, run './make_3ds.sh' and hold enter until you see compiler text.

Literally, I am not kidding. Hold that enter key down. It will still work with you just holding enter.


linux3ds-kernel is based off the ["linux"](https://github.com/linux-3ds/linux) repository from the ["Linux for 3DS" team](https://github.com/linux-3ds) and the [original linux](https://github.com/torvalds/linux) repository.
