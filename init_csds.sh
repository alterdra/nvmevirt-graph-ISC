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
            sudo insmod nvmev0.ko memmap_start=128G memmap_size=10G cpus=1,2
            ;;
        1)
            sudo insmod nvmev1.ko memmap_start=138G memmap_size=10G cpus=3,4
            ;;
        2)
            sudo insmod nvmev2.ko memmap_start=148G memmap_size=10G cpus=5,6
            ;;
        3)
            sudo insmod nvmev3.ko memmap_start=158G memmap_size=10G cpus=7,8
            ;;
    esac
done

# sudo cat /proc/iomem
# Checking valid System RAM

# sudo ./init_csd_edge ../LiveJournal.pl 2 10

