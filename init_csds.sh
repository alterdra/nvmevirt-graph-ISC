#!/bin/bash

# Unload existing kernel modules dynamically
modules=("nvmev0" "nvmev1" "nvmev2" "nvmev3" "nvmev4" "nvmev5" "nvmev6" "nvmev7" "hmb")
for module in "${modules[@]}"; do
    if lsmod | grep -q "$module"; then
        echo "Removing $module..."
        sudo rmmod "$module"
    fi
done

# Parse command-line options
while getopts n:c:p:i:e:v: flag; do
    case "${flag}" in
        n) num_csds=${OPTARG};;  # Number of CSDs
        c) cache_eviction_policy=${OPTARG};;  # Cache policy (FIFO, LRU, etc.)
        p) partial_edge_eviction=${OPTARG};;  # Partial edge eviction flag
        i) invalidation_at_future_value=${OPTARG};; 
        e) edge_buffer_size=${OPTARG};;  # Edge buffer size
        v) vertex_buffer_size=${OPTARG};;  # Vertex buffer size
    esac
done

# Shift processed options, so $1 is the first positional argument
shift $((OPTIND - 1))

# Default number of CSDs if not provided
num_csds=${num_csds:-4}

echo "Number of CSDs: ${num_csds}"

# Clean and rebuild modules
make clean

# Define memory mappings for each module
memmap_start=("128G" "158G" "188G" "218G" "248G" "278G" "308G" "338G")
memmap_size=("30G" "30G" "30G" "30G" "30G" "30G" "30G" "30G")
cpus=("1,2" "3,4" "5,6" "7,8" "9,10" "11,12" "13,14" "15,16")

# Loop to build and load kernel modules
for ID in $(seq 0 $((num_csds - 1))); do
    echo "Building nvmev${ID}..."
    make ID=$ID || exit

    # Load HMB module before first nvmev module
    if [ $ID -eq 0 ]; then
        echo "Loading HMB kernel module..."
        sudo insmod hmb/hmb.ko
    fi

    # Construct optional parameters dynamically
    module_params=""
    [ -n "$cache_eviction_policy" ] && module_params+=" cache_eviction_policy=$cache_eviction_policy"
    [ -n "$partial_edge_eviction" ] && module_params+=" partial_edge_eviction=$partial_edge_eviction"
    [ -n "$invalidation_at_future_value" ] && module_params+=" invalidation_at_future_value=$invalidation_at_future_value"
    [ -n "$edge_buffer_size" ] && module_params+=" edge_buffer_size=$edge_buffer_size"
    [ -n "$vertex_buffer_size" ] && module_params+=" vertex_buffer_size=$vertex_buffer_size"

    # Load nvmev module
    echo "Loading nvmev${ID} with params: memmap_start=${memmap_start[$ID]} memmap_size=${memmap_size[$ID]} cpus=${cpus[$ID]} $module_params"
    sudo insmod nvmev${ID}.ko memmap_start=${memmap_start[$ID]} memmap_size=${memmap_size[$ID]} cpus=${cpus[$ID]} $module_params
done
