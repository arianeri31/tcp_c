#!/bin/bash

NB_SENDS_LIST="500 1000 2500 5000 10000 25000 50000"
SIZES="4096 16384 65536 262144 1048576 4194304 8388608 16777216 33554432"

FIXED_NB_SENDS=500
FIXED_SIZE=65536
POOL_SIZE=16

OUT_SIZE="results_sizes_zc_comp.csv"
OUT_SENDS="results_sends_zc_comp.csv"

HEADER="program,buffer_size,nb_sends,pool_size,total_sent,total_received,total_elapsed_time_us,avg_elapsed_time_us,total_notif,fallback_count"

echo "$HEADER" > "$OUT_SIZE"
echo "$HEADER" > "$OUT_SENDS"

echo "======================================"
echo "ZC PING PONG LOOP CSV TESTS"
echo "Start server_zc_loop before continuing"
echo "........................................"
read -p "Press Enter when server_zc_loop is running..."

echo "........................................"
echo "Running zc_loop tests with several buffer sizes..."
echo

for size in $SIZES; do
    echo "Running client_zc_loop size=$size n=$FIXED_NB_SENDS p=$POOL_SIZE"
    ./client_zc_loop -s "$size" -n "$FIXED_NB_SENDS" -p "$POOL_SIZE" \
        | grep '^RESULT_COMP' \
        | sed 's/^RESULT_COMP,//' \
        >> "$OUT_SIZE"
done

echo "........................................"
echo "Running zc_loop tests with several numbers of sends..."

for sends in $NB_SENDS_LIST; do
    echo "Running client_zc_loop size=$FIXED_SIZE n=$sends p=$POOL_SIZE"
    ./client_zc_loop -s "$FIXED_SIZE" -n "$sends" -p "$POOL_SIZE" \
        | grep '^RESULT_COMP' \
        | sed 's/^RESULT_COMP,//' \
        >> "$OUT_SENDS"
done

echo
echo "======================================"
echo "ZC PIPELINE LOOP CSV TESTS"

echo "........................................"
echo "Running zc_loop_pipeline tests with several buffer sizes..."
echo 

for size in $SIZES; do
    echo "Running client_zc_loop_pipeline size=$size n=$FIXED_NB_SENDS p=$POOL_SIZE"
    ./client_zc_loop_pipeline -s "$size" -n "$FIXED_NB_SENDS" -p "$POOL_SIZE" \
        | grep '^RESULT_COMP' \
        | sed 's/^RESULT_COMP,//' \
        >> "$OUT_SIZE"
done

echo "........................................"
echo "Running zc_loop_pipeline tests with several numbers of sends..."

for sends in $NB_SENDS_LIST; do
    echo "Running client_zc_loop_pipeline size=$FIXED_SIZE n=$sends p=$POOL_SIZE"
    ./client_zc_loop_pipeline -s "$FIXED_SIZE" -n "$sends" -p "$POOL_SIZE" \
        | grep '^RESULT_COMP' \
        | sed 's/^RESULT_COMP,//' \
        >> "$OUT_SENDS"
done

echo "======================================"
echo "Results saved in:"
echo "$OUT_SIZE"
echo "$OUT_SENDS"
echo "======================================"