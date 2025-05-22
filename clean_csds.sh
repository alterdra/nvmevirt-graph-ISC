#!/bin/bash
# Unload existing kernel modules dynamically
modules=()
for i in {0..15}; do
    modules+=("nvmev$i")
done
modules+=("hmb")
for module in "${modules[@]}"; do
    if lsmod | grep -q "^$module"; then
        echo "Removing $module..."
        sudo rmmod "$module"
    fi
done