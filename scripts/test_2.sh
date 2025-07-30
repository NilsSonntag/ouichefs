#!/bin/env bash

set -e
set -u

IMG="../test.img"
MNT="/mnt/ouichefs"
MOD="../ouichefs/ouichefs.ko"
TESTDIR=$(mktemp -d)

echo "Starting OuicheFS test bench (Task 1.2)..."

###########################################
# Helper: test file write/read integrity
test_file() {
	local fname="$1"
	local msg="$2"
	echo -n "$msg" >"$MNT/$fname"
	sync
	local cat_output
	cat_output=$(cat "$MNT/$fname")
	if [ "$cat_output" = "$msg" ]; then
		echo "PASS: $fname"
	else
		echo "FAIL: $fname"
		echo "Expected: $msg"
		echo "Got: $cat_output"
		exit 1
	fi
}

###########################################
# 0. Prepare and mount
insmod "$MOD"
mount "$IMG" "$MNT"

###########################################
# 1. Small file test
test_file "testfile1.txt" "this is a test file!"

# 2. Empty file test
touch "$MNT/empty"
if [ -s "$MNT/empty" ]; then
	echo "FAIL: empty file is not empty"
	exit 1
else
	echo "PASS: empty file"
fi

# 3. Large file test
dd if=/dev/urandom of="$TESTDIR/largefile" bs=4096 count=100 status=none
sync
cp "$TESTDIR/largefile" "$MNT/largefile"
sync
if cmp -s "$TESTDIR/largefile" "$MNT/largefile"; then
	echo "PASS: large file"
else
	echo "FAIL: large file mismatch"
	exit 1
fi

# 4. Block boundary test
msg=$(head -c 4095 </dev/zero | tr '\0' 'A')
test_file "boundaryfile" "$msg" # 4095 A's
msg=$(head -c 4096 </dev/zero | tr '\0' 'B')
test_file "boundaryfile2" "$msg" # 4096 B's

# 5. Random small files
for i in $(seq 1 10); do
	msg="random line $RANDOM"
	test_file "randfile_$i" "$msg"
done

# 6. Performance benchmark (compared to default implementation with page cache)
echo "Benchmarking 10MB sequential write/read..."
start_write=$(date +%s%N)
dd if=/dev/zero of="$MNT/testspeed" bs=1M count=4 oflag=sync status=none
end_write=$(date +%s%N)
our_write_time=$(((end_write - start_write) / 1000000))
echo "Our Write: $our_write_time ms"
sync
start_read=$(date +%s%N)
dd if="$MNT/testspeed" of=/dev/null bs=1M count=4 status=none
end_read=$(date +%s%N)
our_read_time=$(((end_read - start_read) / 1000000))
echo "Our Read: $our_read_time ms"

# Compare to default:
echo "Default OuicheFS Time:"
echo "Write: 110 ms"
echo "Read: 66 ms"

###########################################
# 7. Clean up
sudo umount "$MNT"
sudo rmmod ouichefs
rm -rf "$TESTDIR"

echo "All tests completed."
