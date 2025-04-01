#!/bin/bash
dataset_path=../LiveJournal.pl
num_csds=4
num_iter=10

for aggr_time in 20000 40000 80000 160000 320000; do
    echo "LIFO for aggregation time $aggr_time" >> ./experiments/aggr_LiveJournal.txt
    bash init_csds.sh  -n $num_csds -c LIFO -p 1 -i 1 -e 32M -v 32M
    echo "LIFO for aggregation time $aggr_time"
    cd user
    make
    sudo ./init_csd_edge $dataset_path $num_csds $num_iter $aggr_time >> ../experiments/aggr_LiveJournal.txt
    cd ..
    echo "\n" >> ./experiments/aggr_LiveJournal.txt

    echo "FIFO for aggregation time $aggr_time"  >> ./experiments/aggr_LiveJournal.txt
    bash init_csds.sh  -n $num_csds -e 32M -v 32M
    echo "FIFO for aggregation time $aggr_time"
    cd user
    make
    sudo ./init_csd_edge $dataset_path $num_csds $num_iter $aggr_time  >> ../experiments/aggr_LiveJournal.txt
    cd ..
    echo "\n" >> ./experiments/aggr_LiveJournal.txt
done