#!/bin/bash

# Unload existing kernel modules dynamically
modules=()
for i in {0..23}; do
    modules+=("nvmev$i")
done
modules+=("hmb")
for module in "${modules[@]}"; do
    if lsmod | grep -q "^$module"; then
        echo "Removing $module..."
        sudo rmmod "$module"
    fi
done

make clean

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

total_mem_gb=400
memmap_size_gb=$((total_mem_gb / num_csds))  # 15G per entry

# Initialize arrays
memmap_start=()
memmap_size=()
cpus=()
base_start_gb=128
for ((i = 0; i < num_csds; i++)); do
    start_gb=$((base_start_gb + i * memmap_size_gb))
    memmap_start+=("${start_gb}G")
    memmap_size+=("${memmap_size_gb}G")
    
    # Assign CPU pairs: (2i+1, 2i+2)
    cpu1=$((2 * i + 1))
    cpu2=$((2 * i + 2))
    cpus+=("${cpu1},${cpu2}")
done

# Output results
echo "memmap_start=(${memmap_start[*]})"
echo "memmap_size=(${memmap_size[*]})"
echo "cpus=(${cpus[*]})"

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
