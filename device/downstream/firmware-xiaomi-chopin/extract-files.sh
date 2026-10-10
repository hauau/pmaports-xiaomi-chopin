#!/bin/sh
# SPDX-License-Identifier: MIT
# Copy the stock firmware this package installs out of a Xiaomi chopin vendor
# partition into this directory and verify it against APKBUILD's checksums.
#
# usage: extract-files.sh VENDOR_ROOT
#
# VENDOR_ROOT is the mounted stock vendor partition of MIUI V14.0.6.0.TKPMIXM
# (global): /run/xiaomi-chopin-stock/vendor_a on a phone running this port, or
# a read-only loop mount of vendor_a extracted from the official fastboot ROM.
# The files are proprietary; do not redistribute them.
set -eu

[ $# -eq 1 ] || { echo "usage: $0 VENDOR_ROOT" >&2; exit 2; }
src=$1/firmware
dir=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=APKBUILD
files=$(. "$dir/APKBUILD" && printf '%s\n' $_files)
# shellcheck source=APKBUILD
sums=$(. "$dir/APKBUILD" && printf '%s\n' "$sha512sums" | grep -v '^$')

for file in $files; do
	cp "$src/$file" "$dir/$file"
done
if ! (cd "$dir" && printf '%s\n' "$sums" | sha512sum -c >/dev/null); then
	(cd "$dir" && rm -f $files)
	echo "$0: firmware differs from MIUI V14.0.6.0.TKPMIXM, the only" \
		"vendor release this port was verified with" >&2
	exit 1
fi
echo "Extracted $(printf '%s\n' $files | wc -l) verified firmware files into $dir"
