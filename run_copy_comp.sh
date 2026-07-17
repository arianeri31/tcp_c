#!/bin/bash

NB_SENDS_LIST="500 1000 2500 5000 10000 25000 50000"
SIZES="4096 16384 65536 262144 1048576 4194304 8388608 16777216 33554432"

FIXED_NB_SENDS=500
FIXED_SIZE=65536

OUT_SIZE="results_sizes_copy_comp.csv"
OUT_SENDS="results_sends_copy_comp.csv"

HEADER="program,buffer_size,nb_sends,total_sent,total_received,total_elapsed_time_us,total_msg_time_us,avg_msg_time_us"

echo "$HEADER" > "$OUT_SIZE"
echo "$HEADER" > "$OUT_SENDS"

echo "======================================"
echo "COPY PING PONG LOOP CSV TESTS"
echo "Start server_copy_loop before continuing"
echo "........................................"
read -p "Press Enter when server_copy_loop is running..."

echo "........................................"
echo "Running copy_loop tests with several buffer sizes..."
echo

for size in $SIZES; do
    echo "Running client_copy_loop size=$size n=$FIXED_NB_SENDS"
    ./client_copy_loop -s "$size" -n "$FIXED_NB_SENDS" \
        | grep '^RESULT_COMP_COPY' \
        | sed 's/^RESULT_COMP_COPY,//' \
        >> "$OUT_SIZE"
done

echo "........................................"
echo "Running copy_loop tests with several numbers of sends..."

for sends in $NB_SENDS_LIST; do
    echo "Running client_copy_loop size=$FIXED_SIZE n=$sends"
    ./client_copy_loop -s "$FIXED_SIZE" -n "$sends" \
        | grep '^RESULT_COMP_COPY' \
        | sed 's/^RESULT_COMP_COPY,//' \
        >> "$OUT_SENDS"
done

echo
echo "======================================"
echo "COPY PIPELINE LOOP CSV TESTS"

echo "........................................"
echo "Running copy_loop_pipeline tests with several buffer sizes..."
echo 

for size in $SIZES; do
    echo "Running client_copy_loop_pipeline size=$size n=$FIXED_NB_SENDS"
    ./client_copy_loop_pipeline -s "$size" -n "$FIXED_NB_SENDS" \
        | grep '^RESULT_COMP_COPY' \
        | sed 's/^RESULT_COMP_COPY,//' \
        >> "$OUT_SIZE"
done

echo "........................................"
echo "Running copy_loop_pipeline tests with several numbers of sends..."

for sends in $NB_SENDS_LIST; do
    echo "Running client_copy_loop_pipeline size=$FIXED_SIZE n=$sends"
    ./client_copy_loop_pipeline -s "$FIXED_SIZE" -n "$sends" \
        | grep '^RESULT_COMP_COPY' \
        | sed 's/^RESULT_COMP_COPY,//' \
        >> "$OUT_SENDS"
done

echo "======================================"
echo "Results saved in:"
echo "$OUT_SIZE"
echo "$OUT_SENDS"
echo "======================================"