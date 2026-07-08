#!/bin/bash

NB_SENDS_LIST="500 1000 2500 5000 10000 25000 50000"
SIZES="4096 16384 65536 262144 1048576 4194304 8388608 16777216 33554432"

FIXED_NB_SENDS=500
FIXED_SIZE=65536
POOL_SIZE=1

# echo "======================================"
# echo "COPY LOOP TESTS"
# echo "Start server_copy_loop before continuing"
# echo "........................................"
# read -p "Press Enter when server_copy_loop is running..."

# echo
# echo "......................................."
# echo "copy_loop with several buffer sizes"
# echo "Fixed nb_sends = $FIXED_NB_SENDS"
# echo "======================================"
# echo

# for size in $SIZES; do
#     echo "=== client_copy_loop | size=$size | n=$FIXED_NB_SENDS ==="
#     ./client_copy_loop -s "$size" -n "$FIXED_NB_SENDS"

#     echo
#     echo "**************************************"
#     echo
# done

# echo
# echo "======================================"
# echo "copy_loop with several numbers of sends"
# echo "Fixed buffer_size = $FIXED_SIZE"
# echo "======================================"
# echo

# for sends in $NB_SENDS_LIST; do
#     echo "=== client_copy_loop | size=$FIXED_SIZE | n=$sends ==="
#     ./client_copy_loop -s "$FIXED_SIZE" -n "$sends"

#     echo
#     echo "**************************************"
#     echo
# done

# echo
echo "======================================"
echo "ZC LOOP TESTS"
echo "Stop server_copy_loop and start server_zc_loop before continuing"
echo "........................................"
read -p "Press Enter when server_zc_loop is running..."

echo
echo "........................................"
echo "zc_loop with several buffer sizes"
echo "Fixed nb_sends = $FIXED_NB_SENDS"
echo "Fixed pool_size = $POOL_SIZE"
echo "======================================"
echo

for size in $SIZES; do
    echo "=== client_zc_loop | size=$size | n=$FIXED_NB_SENDS | p=$POOL_SIZE ==="
    ./client_zc_loop -s "$size" -n "$FIXED_NB_SENDS" -p "$POOL_SIZE"

    echo
    echo "**************************************"
    echo
done

echo
echo "======================================"
echo "zc_loop with several numbers of sends"
echo "Fixed buffer_size = $FIXED_SIZE"
echo "Fixed pool_size = $POOL_SIZE"
echo "======================================"
echo

for sends in $NB_SENDS_LIST; do
    echo "=== client_zc_loop | size=$FIXED_SIZE | n=$sends | p=$POOL_SIZE ==="
    ./client_zc_loop -s "$FIXED_SIZE" -n "$sends" -p "$POOL_SIZE"

    echo
    echo "**************************************"
    echo
done