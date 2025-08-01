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

insmod $MOD
mount $DEV $MNT

# echo "===== 1.2: Read & Write ====="
# echo -n "test123" > "$MNT/readtest"
# sync
# read=$(cat "$MNT/readtest")
# assert_eq $read "test123" "1.2: Read & Write"

echo "===== 1.4: Sysfs Checks ====="
for item in free_blocks used_blocks sliced_blocks total_free_slices files small_files total_data_size total_used_size efficiency; do
	[ -f $SYSFS/$item ] && echo "$item: $(cat $SYSFS/$item)"
done

echo "===== 1.5: Write Should Fail for >128 Bytes ====="
dd if=/dev/zero of=$MNT/bigfile bs=129 count=1 2>/dev/null && echo "ERROR: Write >128 bytes should fail!" || echo "Write >128 bytes correctly failed."

echo "===== 1.6: IOCTL Dump Block Slices ====="
echo -n "test123" >"$MNT/ioctltest"
echo -n "file2" >"$MNT/file2"
$IOCTL_HELPER $MNT/ioctltest

sudo umount $MNT
sudo rmmod $MOD

echo "OuicheFS tests completed."
