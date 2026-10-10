#!/bin/sh
# SPDX-License-Identifier: MIT
# Back up the unit-specific factory partitions of a Xiaomi chopin running this
# port: calibration, identifiers and keys that no ROM can restore. Run as root
# on the phone; it only reads, and writes a tar with SHA256SUMS to stdout:
#
#   ssh user@phone 'sudo sh -s' < backup-factory-partitions.sh > chopin-factory.tar
#
# The archive holds this unit's private calibration and identifiers. Keep it
# offline and never publish it.
set -eu

parts="nvram nvdata nvcfg persist proinfo protect1 protect2"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

for part in $parts; do
	dev=/dev/disk/by-partlabel/$part
	[ -b "$dev" ] || { echo "$0: missing $dev" >&2; exit 1; }
	dd if="$dev" of="$dir/$part.img" bs=1M status=none
done
(cd "$dir" && sha256sum *.img > SHA256SUMS)
tar -C "$dir" -cf - .
