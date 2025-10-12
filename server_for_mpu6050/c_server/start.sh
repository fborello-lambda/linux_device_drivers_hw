#!/bin/bash

# Startup script for producer-server system

PRODUCER_PID=""
SERVER_PID=""

cleanup() {
    echo "Cleaning up..."
    if [ ! -z "$SERVER_PID" ]; then
        echo "Stopping server (PID: $SERVER_PID)"
        kill $SERVER_PID 2>/dev/null
    fi
    if [ ! -z "$PRODUCER_PID" ]; then
        echo "Stopping producer (PID: $PRODUCER_PID)"
        kill $PRODUCER_PID 2>/dev/null
    fi
    
    # Clean up shared memory objects
    echo "Cleaning up shared memory..."
    rm -f /dev/shm/data_buffer 2>/dev/null
    rm -f /dev/shm/sem.data_sem 2>/dev/null
    exit 0
}

# Set up signal handlers
trap cleanup SIGINT SIGTERM

echo "Building producer and server..."
make all

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "Starting producer..."
./build/producer &
PRODUCER_PID=$!

# Give producer time to initialize shared memory
sleep 2

echo "Starting server..."
./build/server &
SERVER_PID=$!

echo "Producer PID: $PRODUCER_PID"
echo "Server PID: $SERVER_PID"
PORT=$(cat server_config.cfg | grep port | cut -d'=' -f2)
echo "Server should be available at http://localhost:$PORT"
echo "Press Ctrl+C to stop both processes"

# Wait for either process to exit
wait $PRODUCER_PID $SERVER_PID
cleanup
