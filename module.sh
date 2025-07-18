#!/bin/bash
source /opt/petalinux2021.2/environment-setup-cortexa72-cortexa53-xilinx-linux
make -j32 CROSS_COMPILE=aarch64-xilinx-linux- KBUILD_DIR=~/project/xilinx/linux-xlnx/
