#!/bin/bash

echo "Cleaning old build files..."
sudo make clean

echo "Adding kernel module vtfs..."
sudo make
sudo insmod "source/vtfs.ko"
echo "Mounting kernel module vtfs..."
sudo mount -t vtfs "TODO" /mnt/vt
sudo chmod -R 777 /mnt/vt

echo -e
echo -e
ls -li /mnt
cd /mnt/vt
touch test.txt
ls -lia /mnt/vt
echo -e
echo -e

echo "Removing previous mount..."
cd /mnt/vfs
sudo umount /mnt/vt
sudo rmmod vtfs
