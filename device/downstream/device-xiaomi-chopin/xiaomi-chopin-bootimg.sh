#!/bin/sh
set -eu

# boot-deploy passes the generated initramfs path; boot.img is beside it.
avbtool add_hash_footer \
	--image "$(dirname "$1")/boot.img" \
	--partition_name boot --partition_size 134217728 \
	--algorithm NONE --hash_algorithm sha256 \
	--prop com.android.build.boot.os_version:12 \
	--prop com.android.build.boot.security_patch:2024-04-01
