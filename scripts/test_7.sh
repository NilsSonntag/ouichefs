#!/bin/env bash
set -e

MNT=/mnt/ouichefs
DEV=../test.img
MOD=../ouichefs/ouichefs.ko
SYSFS=/sys/fs/ouichefs/loop0
IOCTL_HELPER=./ioctl_dump

# functions
cleanup() {
  umount $MNT 2>/dev/null || true
  rmmod $MOD 2>/dev/null || true
}
trap cleanup EXIT

fail() {
  echo "Test FAILED: $1"
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

insmod $MOD
mount $DEV $MNT

# echo "===== 1.2: Read & Write ====="
echo -n "readtest" >"$MNT/readtest"
# sync
# read=$(cat "$MNT/readtest")
# assert_eq "$read" "test123" "1.2: Read & Write"

echo "===== 1.4: Sysfs Checks ====="
sysfs_asserts=(
  "free_blocks:12543"
  "used_blocks:1"
  "sliced_blocks:1"
  "total_free_slices:30"
  "files:1"
  "small_files:1"
  "total_data_size:8"
  "total_used_size:4096"
  "efficiency:0"
)

for item in "${sysfs_asserts[@]}"; do
  name="${item%%:*}"
  expected="${item#*:}"
  value=$(cat "$SYSFS/$name")
  assert_eq "$value" "$expected" "$name"
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
dump_bitmap="$($IOCTL_HELPER "$MNT/ioctltest" | head -n 1)"
assert_eq $dump_bitmap "fffffff8" "Bitmap 2 files"
echo -n "file2" >"$MNT/file2"
echo -n "456test" >"$MNT/file2"
dump_bitmap="$($IOCTL_HELPER "$MNT/file2" | head -n 1)"
assert_eq $dump_bitmap "fffffff0" "Bitmap 3 files"

echo "===== 1.7: Deletion and Slice Reuse ====="
echo -n "delete_me" >"$MNT/todelete"
before_free_slices=$(cat $SYSFS/total_free_slices)
before_sliced_blocks=$(cat $SYSFS/sliced_blocks)
assert_eq $before_free_slices 27 "Free slices before deletion"
assert_eq $before_sliced_blocks 1 "Sliced blocks before deletion"
rm "$MNT/todelete"
sync
after_free_slices=$(cat $SYSFS/total_free_slices)
after_sliced_blocks=$(cat $SYSFS/sliced_blocks)
assert_eq $after_free_slices 28 "Free slices after deletion"
assert_eq $after_sliced_blocks 1 "Sliced blocks after deletion"

# Try creating a new file and ensure it reuses freed slice
echo -n "reuse" >"$MNT/reusefile"
dump_bitmap="$($IOCTL_HELPER "$MNT/reusefile" | head -n 1)"
assert_eq $dump_bitmap "ffffffe0" "Bitmap after delete"

cleanup

echo "OuicheFS tests completed."
