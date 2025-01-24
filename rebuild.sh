#!/bin/bash

echo "Cleaning previous build..."
sudo make clean

echo "Building with DEBUG=1..."
sudo make DEBUG=1 -j 30

echo "Installing with DEBUG=1..."
sudo make install DEBUG=1 -j 30

echo "Build process completed!"
