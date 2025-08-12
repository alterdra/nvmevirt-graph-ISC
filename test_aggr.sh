#!/bin/bash

# Ex: bash test_aggr.sh LiveJournal.pl 3 526M 18M
# Ex: bash test_aggr.sh Twitter-2010.pl 1 11G 160M
# Ex: bash test_aggr.sh Friendster.pl 1 14G 250M
# Ex: bash test_aggr.sh ./storage_sdf/lumos/Uk-2007.pl 1 30G 414M
# Ex: bash test_aggr.sh ./storage_sdf/lumos/RMAT29.pl 5 66G 537M

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
vertex_size_human=$4

edge_size=$(convert_to_bytes $edge_size_human)
vertex_size=$(convert_to_bytes $vertex_size_human)
x_decimal=$(echo "scale=4; $x_percentage / 100" | bc | sed 's/^\./0./')

edge_alloc=$(echo "scale=4; $edge_size * $x_decimal" | bc)
edge_alloc=$(echo "$edge_alloc" | awk '{print int($1)}')
# edge_alloc_human=$(convert_to_human $edge_alloc)
edge_alloc_human="1G"

vertex_alloc=$(echo "scale=4; $vertex_size * 2 * $x_decimal" | bc)
vertex_alloc=$(echo "$vertex_alloc" | awk '{print int($1)}')
vertex_alloc_human=$(convert_to_human $vertex_alloc)

num_partition=$(awk 'NR==1{print $4}' "$dataset_path/meta")
echo "Number of partitions: $num_partition"

echo "Output path: $dataset_path"
cleaned_path="${dataset_path##*/}"
echo "Cleaned path: $cleaned_path"

output_path="experiments/cache_eviction/aggr_${cleaned_path}_p${num_partition}_c${num_csd}.txt"

cd user
make
cd ..

# 20000 40000 80000 160000 320000
algorithm="PR"

# LIFO
bash init_csds.sh -n $num_csd -c PRIORITY -p 1 -i 1 -e $edge_alloc_human -v $vertex_alloc_human
for aggr_time in 10000 20000 30000; do
    echo "PRIORITY for aggregation time $aggr_time. Allocating: edge_size=$edge_alloc_human vertex_size=$vertex_alloc_human $for num_csd=$num_csd"
    echo "PRIORITY for aggregation time $aggr_time" >> $output_path
    sudo ./user/init_csd_edge $dataset_path $num_csd $algorithm $num_iter $aggr_time >> $output_path
    printf "\n" >> $output_path
done

# FIFO
bash init_csds.sh  -n $num_csd -e $edge_alloc_human -v $vertex_alloc_human
for aggr_time in 10000 20000 30000; do
    echo "FIFO for aggregation time $aggr_time. Allocating: edge_size=$edge_alloc_human for num_csd=$num_csd"
    echo "FIFO for aggregation time $aggr_time" >> $output_path
    sudo ./user/init_csd_edge $dataset_path $num_csd $algorithm $num_iter $aggr_time >> $output_path
    printf "\n" >> $output_path
done