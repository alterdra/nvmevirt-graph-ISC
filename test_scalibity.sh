#!/bin/bash

# Ex: bash test_scalibity.sh LiveJournal.pl 5 526M 18M
# Ex: bash test_scalibity.sh Twitter-2010.pl 5 11G 160M
# Ex: bash test_scalibity.sh Friendster.pl 5 14G 250M
# Ex: bash test_scalibity.sh Uk-2007.pl 5 30G 414M

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

edge_size=$(convert_to_bytes $edge_size_human)
vertex_size=$(convert_to_bytes $vertex_size_human)
x_decimal=$(echo "scale=4; $x_percentage / 100" | bc | sed 's/^\./0./')

edge_alloc=$(echo "scale=4; $edge_size * $x_decimal" | bc)
edge_alloc=$(echo "$edge_alloc" | awk '{print int($1)}')
edge_alloc_human=$(convert_to_human $edge_alloc)

vertex_alloc=$(echo "scale=4; $vertex_size * 2 * $x_decimal" | bc)
vertex_alloc=$(echo "$vertex_alloc" | awk '{print int($1)}')
vertex_alloc_human=$(convert_to_human $vertex_alloc)

num_partition=$(awk 'NR==1{print $4}' "$dataset_path/meta")
echo "Number of partitions: $num_partition"
output_path="experiments/scaling_${dataset_path}_${x_percentage}%_p${num_partition}.txt"

cd user
make
cd ..

total_num_csd=8
echo "Allocating: edge_size=$edge_alloc_human, vertex_size=$vertex_alloc_human for num_csd=$total_num_csd"
if [ "$x_percentage" -eq 100 ]; then
    bash init_csds.sh -n $total_num_csd
else
    bash init_csds.sh -n $total_num_csd -e $edge_alloc_human -v $vertex_alloc_human
fi

# Loop through the number of CSDs
for num_csd in 1 2 4 8; do
    if [ "$num_csd" -eq 1 ]; then
        aggr_latency=0
    else
        aggr_latency=10000
    fi
    sudo ./user/init_csd_edge $dataset_path $num_csd 10 $aggr_latency >> $output_path

done
