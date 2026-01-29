#!/bin/bash

echo "Cleaning old build files..."
sudo make clean

echo "Adding kernel module vtfs..."
sudo make
sudo insmod "vtfs.ko"
echo "Mounting kernel module vtfs..."
sudo mount -t vtfs "TODO" /mnt/vt
