#!/bin/bash

NB_SENDS_LIST="500 1000 2500 5000 10000"
SIZES="4096 16384 65536 262144 1048576"

OUT_SIZE="results_sizes_simple.csv"
OUT_SENDS="results_sends_simple.csv"

HEADER="program,buffer_size,nb_sends,total_sent,total_elapsed_time_us,avg_send_us,min_send_us,max_send_us,total_notif,fallback_count"

echo "$HEADER" > "$OUT_SIZE"

for size in $SIZES; do
    echo "Running client_copy size=$size n=500"
    ./client_copy -s "$size" -n 500 | grep '^RESULT' | sed 's/^RESULT,//' >> "$OUT_SIZE"

    echo "Running client_zc size=$size n=500"
    ./client_zc -s "$size" -n 500 | grep '^RESULT' | sed 's/^RESULT,//' >> "$OUT_SIZE"
done

echo "Results saved in $OUT_SIZE"

echo "$HEADER" > "$OUT_SENDS"

for sends in $NB_SENDS_LIST; do
    echo "Running client_copy size=65536 n=$sends"
    ./client_copy -s 65536 -n "$sends" | grep '^RESULT' | sed 's/^RESULT,//' >> "$OUT_SENDS"

    echo "Running client_zc size=65536 n=$sends"
    ./client_zc -s 65536 -n "$sends" | grep '^RESULT' | sed 's/^RESULT,//' >> "$OUT_SENDS"
done

echo "Results saved in $OUT_SENDS"