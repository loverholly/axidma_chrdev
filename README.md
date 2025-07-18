# axidma_chrdev
chrdev of axidma
1. petalinux environments
   cd project-spec/meta-user/recipes-modules/xilinx-axidma

   files/Makefile
   obj-m := xilinx-axidma.o
   xilinx-axidma-objs := axi_dma.o axidma_chrdev.o axidma_dma.o axidma_of.o

   .bb
   SRC_URI = "file://Makefile \
           file://axi_dma.c \
           file://axidma_chrdev.c \
           file://axidma_dma.c \
           file://axidma_of.c \
           file://axidma.h \
           file://axidma_ioctl.h \
           file://COPYING"

	build command
	petalinux-build -c xilinx-axidma
2. just the xilinx linux environment
