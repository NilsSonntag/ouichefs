#!/bin/env bash
set -e

MNT=/mnt/ouichefs
DEV=../test.img
MOD=../ouichefs/ouichefs.ko
SYSFS=/sys/fs/ouichefs/loop0
IOCTL_HELPER=./ioctl_dump

fail() {
  echo "Test FAILED: $1"
  sudo umount "$MNT" || true
  sudo rmmod "$MOD" || true
  exit 1
}

assert_eq() {
  local value="$1"
  local expected="$2"
  local name="$3"
  if [ "$value" != "$expected" ]; then
    fail "$name: expected $expected but got $value"
  else
    echo "PASS: $name = $value"
  fi
}

sudo insmod $MOD
sudo mount $DEV $MNT

# echo "===== 1.2: Read & Write ====="
# echo -n "test123" >"$MNT/readtest"
# sync
# read=$(cat "$MNT/readtest")
# assert_eq "$read" "test123" "1.2: Read & Write"

echo "===== 1.4: Sysfs Checks ====="
for item in free_blocks used_blocks sliced_blocks total_free_slices files small_files total_data_size total_used_size efficiency; do
  assert_eq "$(cat $SYSFS/$item)" "$item"
done

echo "===== 1.5: Write Should Fail for >128 Bytes ====="
dd if=/dev/zero of=$MNT/bigfile bs=129 count=1 2>/dev/null && fail "1.5: Write >128 bytes should fail!" || echo "Write >128 bytes correctly failed."

# echo "===== 1.5: Overwrite, Append, and Partial Write (all <128) ====="
# echo -n "one" >"$MNT/fileA"
# echo -n "two" >"$MNT/fileA"
# val=$(cat "$MNT/fileA")
# assert_eq "$val" "two" "1.5: Overwrite small file"
#
# echo -n "abc" >>"$MNT/fileA"
# val=$(cat "$MNT/fileA")
# assert_eq "$val" "twoabc" "1.5: Append to small file"
#
# dd if=/dev/zero of="$MNT/fileA" bs=1 seek=2 count=2 conv=notrunc 2>/dev/null
# val=$(cat "$MNT/fileA")
# # Should be "to\x00\x00abc" if null bytes are written
# echo "After dd overwrite: $val"

echo "===== 1.6: IOCTL Dump Block Slices ====="
echo -n "test123" >"$MNT/ioctltest"
$IOCTL_HELPER "$MNT/ioctltest" || fail "1.6: IOCTL dump failed"
echo -n "file2" >"$MNT/file2"
echo -n "456test" >"$MNT/file2"
$IOCTL_HELPER "$MNT/file2" || fail "1.6: IOCTL dump for file2 failed"

echo "===== 1.7: Deletion and Slice Reuse ====="
echo -n "delete_me" >"$MNT/todelete"
# Get sysfs stats before deletion
before_slices=$(cat $SYSFS/total_free_slices)
before_sliced_blocks=$(cat $SYSFS/sliced_blocks)
rm "$MNT/todelete"
sync
# After deletion, free slices count should increase, possibly sliced_blocks decrease if block is fully freed.
after_slices=$(cat $SYSFS/total_free_slices)
after_sliced_blocks=$(cat $SYSFS/sliced_blocks)
echo "Slices before: $before_slices, after: $after_slices"
echo "Sliced blocks before: $before_sliced_blocks, after: $after_sliced_blocks"
if [ "$after_slices" -le "$before_slices" ]; then
  fail "1.7: Slices not freed on file deletion"
fi

# Try creating a new file and ensure it reuses freed slice
echo -n "reuse" >"$MNT/reusefile"
$IOCTL_HELPER "$MNT/reusefile" || fail "1.7: IOCTL after slice reuse failed"

# Clean up
sudo umount $MNT
sudo rmmod $MOD

echo "OuicheFS tests completed."
