#!/bin/bash
set -e

mnt=/tmp/mnt1
dev=vda
modulename=ouichefs
sysfs="/sys/fs/ouichefs"

# Make sure mount point exists
mkdir -p $mnt

echo "Loading OuicheFS module..."
insmod /home/niels/git/ouichefs/ouichefs.ko || true

echo "Mounting OuicheFS..."
mount /dev/$dev $mnt

# Clean up any old files
rm -rf $mnt/*

echo "Creating random files..."
for i in $(seq 1 100); do
  # random size: 1-4000 bytes
  size=$(((RANDOM % 4000) + 1))
  dd if=/dev/urandom of=$mnt/file_$i bs=1 count=$size status=none
done

# Wait for disk flush
sync

echo
echo "OuicheFS sysfs stats for /dev/$dev:"
for f in total_data_size total_used_size efficiency files small_files; do
  printf "%-18s: " "$f"
  cat "$sysfs/$dev/$f"
done

umount $mnt
rmmod $modulename
