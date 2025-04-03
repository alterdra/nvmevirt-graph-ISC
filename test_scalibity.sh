#!/bin/bash
bash init_csds.sh -n 1
dataset_path=$1
cd user
make
for i in 1; do
    if [ "$i" -eq 1 ]; then
        sudo ./init_csd_edge $dataset_path $i 10 0
    else
        sudo ./init_csd_edge $dataset_path $i 10 20000
    fi
done