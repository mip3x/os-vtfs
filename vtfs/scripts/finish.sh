#!/bin/bash

echo "Removing previous mount..."
sudo umount /mnt/vt
sudo rmmod vtfs
