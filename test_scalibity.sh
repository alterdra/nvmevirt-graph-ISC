#!/bin/bash
bash init_csds.sh -n 8
dataset_path=$1
cd user
make
for i in 1 2 4 8; do
    sudo ./init_csd_edge $dataset_path $i 10 20000
done