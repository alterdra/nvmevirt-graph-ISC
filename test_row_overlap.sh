#!/bin/bash

# Ex: bash test_row_overlap.sh LiveJournal.pl 526M 18M
# Ex: bash test_row_overlap.sh Twitter-2010.pl 11G 160M
# Ex: bash test_row_overlap.sh Friendster.pl 14G 250M
# Ex: bash test_row_overlap.sh ./storage_sdf/lumos/Uk-2007.pl 30G 414M
# Ex: bash test_row_overlap.sh ./storage_sdf/lumos/RMAT29.pl 66G 537M

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
edge_size_human=$2
vertex_size_human=$3

edge_size=$(convert_to_bytes $edge_size_human)
vertex_size=$(convert_to_bytes $vertex_size_human)
x_decimal=0.05

edge_alloc=$(echo "scale=4; $edge_size * $x_decimal" | bc)
edge_alloc=$(echo "$edge_alloc" | awk '{print int($1)}')
# edge_alloc_human=$(convert_to_human $edge_alloc)
edge_alloc_human="100M"

vertex_alloc=$(echo "scale=4; $vertex_size * 2 * $x_decimal" | bc)
vertex_alloc=$(echo "$vertex_alloc" | awk '{print int($1)}')
vertex_alloc_human=$(convert_to_human $vertex_alloc)

num_partition=$(awk 'NR==1{print $4}' "$dataset_path/meta")
echo "Number of partitions: $num_partition"

echo "Output path: $dataset_path"
cleaned_path="${dataset_path##*/}"
echo "Cleaned path: $cleaned_path"

output_path="experiments/row_overlap/row_overlap_${cleaned_path}_p${num_partition}_${edge_alloc_human}.txt"

cd user
make
cd ..

num_csd=8
echo "Allocating: edge_size=$edge_alloc_human, vertex_size=$vertex_alloc_human for num_csd=$num_csd"
bash init_csds.sh -n $num_csd -c PRIORITY -p 1 -i 1 -e $edge_alloc_human -v $vertex_alloc_human
sudo ./user/init_csd_edge $dataset_path $num_csd 10 >> $output_path