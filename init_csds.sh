#!/bin/bash

#sudo rmmod nvmev

TARGET=$1

#echo "Load nvme module"
make clean
make ID=$TARGET || exit

echo "Load NVMeVirt kernel module...", $TARGET

if [ $TARGET -eq 0 ]
then
	if lsmod | grep -q nvmev0; then
        sudo rmmod nvmev0
    fi

	sudo insmod nvmev0.ko memmap_start=1G memmap_size=1G cpus=1,2

elif [ $TARGET -eq 1 ]
then
	if lsmod | grep -q nvmev1; then
        sudo rmmod nvmev1
    fi

	sudo insmod nvmev1.ko memmap_start=2G memmap_size=1G cpus=3,4
fi