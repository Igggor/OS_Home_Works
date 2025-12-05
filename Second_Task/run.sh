#!/bin/bash

MODE="$1"

if [ -z "$MODE" ]; then
    echo "Usage: ./run.sh <mode>"
    echo "Modes: pipe | fifo | msg | shm | sock | file_sem"
    exit 1
fi

case "$MODE" in
    pipe|fifo|msg|shm|sock|file_sem)
        ;;
    *)
        echo "Invalid mode: $MODE"
        echo "Valid: pipe | fifo | msg | shm | sock | file_sem"
        exit 1
        ;;
esac

export TABLE_LIMIT=5

./dishwasher "$MODE" wash_times.txt dry_times.txt dirty.txt
