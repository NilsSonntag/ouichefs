#!/usr/bin/env bash

set -e

MOD="../ouichefs/ouichefs.ko"
DEV="../test.img"
MNT="/mnt/ouichefs"
SYSFS="/sys/fs/ouichefs/loop0"

fail() {
  echo "Test FAILED: $1"
  sudo umount "$MNT" || true
  exit 1
}

assert_eq() {
  local value="$1"
  local expected="$2"
  local name="$3"
  if [ "$value" != "$expected" ]; then
    fail "$name: expected $expected but got $value"
    exit 1
  else
    echo "PASS: $name = $value"
  fi
}

get_stat() {
  local stat=$1
  cat "$SYSFS/$stat"
}

insmod $MOD
mount $DEV $MNT

# Clean mountpoint
rm -f $MNT/*

free_blocks=$(get_stat free_blocks)
used_blocks=$(get_stat used_blocks)
sliced_blocks=$(get_stat sliced_blocks)
total_free_slices=$(get_stat total_free_slices)
files=$(get_stat files)
small_files=$(get_stat small_files)
total_data_size=$(get_stat total_data_size)
total_used_size=$(get_stat total_used_size)
efficiency=$(get_stat efficiency)

echo "Asserting sysfs values BEFORE file creation..."

assert_eq "$free_blocks" 12544 "free_blocks"
assert_eq "$used_blocks" 0 "used_blocks"
assert_eq "$sliced_blocks" 0 "sliced_blocks"
assert_eq "$total_free_slices" 0 "total_free_slices"
assert_eq "$files" 0 "files"
assert_eq "$small_files" 0 "small_files"
assert_eq "$total_data_size" 0 "total_data_size"
assert_eq "$total_used_size" 0 "total_used_size"
assert_eq "$efficiency" 0 "efficiency"

echo "Creating files..."
# Create small files
echo "hello" >$MNT/f1
echo "world" >$MNT/f2

# Create larger file
dd if=/dev/zero of=$MNT/bigfile bs=512 count=10 status=none

sync

echo "Asserting sysfs values AFTER file creation..."

free_blocks=$(get_stat free_blocks)
used_blocks=$(get_stat used_blocks)
sliced_blocks=$(get_stat sliced_blocks)
total_free_slices=$(get_stat total_free_slices)
files=$(get_stat files)
small_files=$(get_stat small_files)
total_data_size=$(get_stat total_data_size)
total_used_size=$(get_stat total_used_size)
efficiency=$(get_stat efficiency)

echo "free_blocks" "$free_blocks"
echo "used_blocks" "$used_blocks"
echo "sliced_blocks" "$sliced_blocks"
echo "total_free_slices" "$total_free_slices"
assert_eq "$files" 3 "files"
assert_eq "$small_files" 2 "small_files"
echo "total_data_size" "$total_data_size"
echo "total_used_size" "$total_used_size"
echo "efficiency" "$efficiency"

echo "Deleting file f1..."
rm $MNT/f1
sync

# Gather stats after deletion
free_blocks=$(get_stat free_blocks)
used_blocks=$(get_stat used_blocks)
sliced_blocks=$(get_stat sliced_blocks)
total_free_slices=$(get_stat total_free_slices)
files=$(get_stat files)
small_files=$(get_stat small_files)
total_data_size=$(get_stat total_data_size)
total_used_size=$(get_stat total_used_size)
efficiency=$(get_stat efficiency)

echo "Asserting sysfs values after deleting f1..."
echo "free_blocks" "$free_blocks"
echo "used_blocks" "$used_blocks"
echo "sliced_blocks" "$sliced_blocks"
echo "total_free_slices" "$total_free_slices"
assert_eq "$files" 2 "files"
assert_eq "$small_files" 1 "small_files"
echo "total_data_size" "$total_data_size"
echo "total_used_size" "$total_used_size"
echo "efficiency" "$efficiency"

sudo umount $MNT
echo "All tests PASSED."
