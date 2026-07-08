#!/bin/bash

NB_SENDS_LIST="500 1000 2500 5000 10000"
SIZES="4096 16384 65536 262144 1048576"

FIXED_NB_SENDS=500
FIXED_SIZE=65536

echo "======================================"
echo "Tests with several buffer sizes"
echo "Fixed nb_sends = $FIXED_NB_SENDS"
echo "======================================"
echo

for size in $SIZES; do
    echo "=== client_copy | size=$size | n=$FIXED_NB_SENDS ==="
    ./client_copy -s "$size" -n "$FIXED_NB_SENDS"

    echo

    echo "=== client_zc | size=$size | n=$FIXED_NB_SENDS ==="
    ./client_zc -s "$size" -n "$FIXED_NB_SENDS"

    echo
    echo "**************************************"
    echo
done

echo "======================================"
echo "Tests with several numbers of sends"
echo "Fixed buffer_size = $FIXED_SIZE"
echo "======================================"
echo

for sends in $NB_SENDS_LIST; do
    echo "=== client_copy | size=$FIXED_SIZE | n=$sends ==="
    ./client_copy -s "$FIXED_SIZE" -n "$sends"

    echo

    echo "=== client_zc | size=$FIXED_SIZE | n=$sends ==="
    ./client_zc -s "$FIXED_SIZE" -n "$sends"

    echo
    echo "**************************************"
    echo
done
echo "======================================"