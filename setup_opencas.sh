#!/bin/bash

# Check if script is run as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (sudo)"
    exit 1
fi

# Create RAM disk (10GB)
echo "Creating RAM disk..."
modprobe brd rd_size=1048576
ln -s /dev/ram1 /dev/disk/by-id/ramdisk1

# Verify RAM disk creation
if [ ! -e /dev/ram1 ]; then
    echo "Failed to create RAM disk"
    exit 1
fi
echo "RAM disk created successfully at /dev/ram1"

# Verify NVMe device exists
if [ ! -e /dev/nvme1n1 ]; then
    echo "NVMe device /dev/nvme1n1 not found"
    exit 1
fi
echo "Found NVMe device at /dev/nvme1n1"

# Stop existing cache instance if any
echo "Stopping any existing cache instance..."
casadm -S -i 1 2>/dev/null

# Create new cache instance
echo "Creating new cache instance..."
if ! casadm -S -i 1 -d /dev/ram1 -c wt; then
    echo "Failed to create cache instance"
    exit 1
fi

# Add NVMe as core device
echo "Adding NVMe as core device..."
if ! casadm -A -i 1 -j 1 -d /dev/disk/by-id/nvme-Linux_1ec6b3b12c4a3d04; then
    echo "Failed to add core device"
    casadm -S -i 1  # Stop cache instance on failure
    exit 1
fi

# Verify setup
echo "Verifying setup..."
casadm -L

echo "Setup completed successfully!" 