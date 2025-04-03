#!/bin/bash

# Ex: bash test_scalibity.sh LiveJournal.pl 25 526M 18M

# Function to convert human-readable sizes (K, M, G) to bytes
convert_to_bytes() {
    local size=$1
    case $size in
        *K) echo $(( ${size%K} * 1024 )) ;;
        *M) echo $(( ${size%M} * 1024 * 1024 )) ;;
        *G) echo $(( ${size%G} * 1024 * 1024 * 1024 )) ;;
        *) echo $size ;; # Assume it's already in bytes if no suffix
    esac
}

# Function to convert bytes back to human-readable format
convert_to_human() {
    local bytes=$1
    if (( bytes >= 1024 * 1024 * 1024 )); then
        echo "$(( bytes / (1024 * 1024 * 1024) ))G"
    elif (( bytes >= 1024 * 1024 )); then
        echo "$(( bytes / (1024 * 1024) ))M"
    elif (( bytes >= 1024 )); then
        echo "$(( bytes / 1024 ))K"
    else
        echo "${bytes}B"
    fi
}

# Read input arguments
dataset_path=$1
x_percentage=$2
edge_size_human=$3
vertex_size_human=$4

# Convert edge_size and vertex_size to bytes
edge_size=$(convert_to_bytes $edge_size_human)
vertex_size=$(convert_to_bytes $vertex_size_human)

# Ensure x_percentage is in decimal form
x_decimal=$(echo "scale=4; $x_percentage / 100" | bc | sed 's/^\./0./')

cd user
make
cd ..

# Loop through the number of CSDs
for num_csd in 1 2 4 8; do
    edge_alloc=$(echo "scale=4; $edge_size * $x_decimal / $num_csd" | bc)
    vertex_alloc=$(echo "scale=4; $vertex_size * 2 * $x_decimal / $num_csd" | bc)
    edge_alloc=$(echo "$edge_alloc" | awk '{print int($1)}')
    vertex_alloc=$(echo "$vertex_alloc" | awk '{print int($1)}')
    # Convert back to human-readable format
    edge_alloc_human=$(convert_to_human $edge_alloc)
    vertex_alloc_human=$(convert_to_human $vertex_alloc)

    echo "Allocating: edge_size=$edge_alloc_human, vertex_size=$vertex_alloc_human for num_csd=$num_csd"

    if [ "$x_percentage" -eq 100 ]; then
        bash init_csds.sh -n $num_csd
    else
        bash init_csds.sh -n $num_csd -e $edge_alloc_human -v $vertex_alloc_human
    fi

    if [ "$num_csd" -eq 1 ]; then
        aggr_latency=0
    else
        aggr_latency=20000
    fi

    sudo ./user/init_csd_edge $dataset_path $num_csd 10 $aggr_latency >> experiments/aggr_${dataset_path}_${x_percentage}.txt
done
