#!/bin/bash

# Ex: bash test_aggr.sh Twitter-2010.pl 25 11G

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

num_csd=8
num_iter=10

dataset_path=$1
x_percentage=$2
edge_size_human=$3

edge_size=$(convert_to_bytes $edge_size_human)
x_decimal=$(echo "scale=4; $x_percentage / 100" | bc | sed 's/^\./0./')

edge_alloc=$(echo "scale=4; $edge_size * $x_decimal / $num_csd" | bc)
edge_alloc=$(echo "$edge_alloc" | awk '{print int($1)}')
edge_alloc_human=$(convert_to_human $edge_alloc)

# 20000 40000 80000 160000 320000

# LIFO
bash init_csds.sh -n $num_csd -c LIFO -p 1 -i 1 -e $edge_alloc_human
for aggr_time in 20000 40000 80000 160000 320000 640000; do
    echo "LIFO for aggregation time $aggr_time"
    echo "Allocating: edge_size=$edge_alloc_human for num_csd=$num_csd"
    cd user
    make
    cd ..
    echo "LIFO for aggregation time $aggr_time" >> ./experiments/aggr_${dataset_path}_${x_percentage}.txt
    sudo ./user/init_csd_edge $dataset_path $num_csd $num_iter $aggr_time >> ./experiments/aggr_${dataset_path}_${x_percentage}.txt
    printf "\n" >> ./experiments/aggr_${dataset_path}_${x_percentage}.txt
done

# FIFO
bash init_csds.sh  -n $num_csd -e $edge_alloc_human
for aggr_time in 20000 40000 80000 160000 320000 640000; do
    echo "FIFO for aggregation time $aggr_time"
    echo "Allocating: edge_size=$edge_alloc_human for num_csd=$num_csd"
    cd user
    make
    cd ..
    echo "FIFO for aggregation time $aggr_time" >> ./experiments/aggr_${dataset_path}_${x_percentage}.txt
    sudo ./user/init_csd_edge $dataset_path $num_csd $num_iter $aggr_time >> ./experiments/aggr_${dataset_path}_${x_percentage}.txt
    printf "\n" >> ./experiments/aggr_${dataset_path}_${x_percentage}.txt
done