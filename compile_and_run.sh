#!/bin/bash

echo "==================================="
echo "Virtual Memory Manager - Build Script"
echo "==================================="

# Compile main program
echo "Compiling vmm.c..."
gcc -o vmm vmm.c -lm -w
if [ $? -ne 0 ]; then
    echo "Error: Failed to compile vmm.c"
    exit 1
fi
echo "Successfully compiled vmm"

# Create programs directory if it doesn't exist
mkdir -p programs

# Check if config.txt exists
if [ ! -f config.txt ]; then
    echo "Creating default config.txt..."
    echo "64" > config.txt
    echo "4" >> config.txt
    echo "Created config.txt with 64KB memory and 4KB pages"
fi

echo ""
echo "==================================="
echo "Running Virtual Memory Manager..."
echo "==================================="
echo ""

# Run the program
./vmm

echo ""
echo "==================================="
echo "Done! Check visualization.html"
echo "==================================="