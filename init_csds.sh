#!/bin/bash
if lsmod | grep -q nvmev0; then
    sudo rmmod nvmev0
fi
if lsmod | grep -q nvmev1; then
    sudo rmmod nvmev1
fi
if lsmod | grep -q nvmev2; then
    sudo rmmod nvmev2
fi
if lsmod | grep -q nvmev3; then
    sudo rmmod nvmev3
fi
if lsmod | grep -q nvmev4; then
    sudo rmmod nvmev4
fi
if lsmod | grep -q nvmev5; then
    sudo rmmod nvmev5
fi
if lsmod | grep -q nvmev6; then
    sudo rmmod nvmev6
fi
if lsmod | grep -q nvmev7; then
    sudo rmmod nvmev7
fi
if lsmod | grep -q hmb; then
    sudo rmmod hmb
fi


make clean

number_of_CSDs=$1
echo "Number of CSDs: ${number_of_CSDs}"

# Loop for IDs 0 and 1
for ID in `seq 0 $((number_of_CSDs - 1))`; do
    echo "Building nvmev${ID}..."
    make ID=$ID || exit

    # Only load HMB once, before the first module
    if [ $ID -eq 0 ]; then
        echo "Load HMB kernel module..."
        sudo insmod hmb/hmb.ko
    fi

    echo "Loading nvmev${ID}..."
    case $ID in
        0)
            sudo insmod nvmev0.ko memmap_start=128G memmap_size=30G cpus=1,2
            ;;
        1)
            sudo insmod nvmev1.ko memmap_start=158G memmap_size=30G cpus=3,4
            ;;
        2)
            sudo insmod nvmev2.ko memmap_start=188G memmap_size=30G cpus=5,6
            ;;
        3)
            sudo insmod nvmev3.ko memmap_start=218G memmap_size=30G cpus=7,8
            ;;
        4)
            sudo insmod nvmev4.ko memmap_start=248G memmap_size=30G cpus=9,10
            ;;
        5)
            sudo insmod nvmev5.ko memmap_start=278G memmap_size=30G cpus=11,12
            ;;
        6)
            sudo insmod nvmev6.ko memmap_start=308G memmap_size=30G cpus=13,14
            ;;
        7)
            sudo insmod nvmev7.ko memmap_start=338G memmap_size=30G cpus=15,16
            ;;
    esac
done

# sudo cat /proc/iomem
# Checking valid System RAM

# sudo ./init_csd_edge ../LiveJournal.pl 2 10

