#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Add the Android storage group IDs to pmOS so the desktop user can read
# shared storage and read-only Android partitions.
#
# Android uses fixed numeric GIDs; the names are only cosmetic.  pmOS has no
# groups in this range, so add them and make "user" a member.  Idempotent.

set -eu

# systemd and chroot invocations do not always provide a full PATH.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

GROUPS="1000:system 1015:sdcard_rw 1023:media_rw 1028:sdcard_r 1078:ext_data_rw 1079:ext_obb_rw"
TMP=/etc/group.pmos-new

for pair in $GROUPS; do
	gid=${pair%%:*}
	name=${pair#*:}

	if grep -q "^[^:]*:x:$gid:" /etc/group; then
		# Existing group: add user to the member list if needed.
		if ! awk -F: -v gid="$gid" '$3 == gid { print $4 }' /etc/group |
			tr ',' '\n' | grep -qx user; then
			awk -F: -v OFS=: -v gid="$gid" '
				$3 == gid {
					if ($4 == "")
						$4 = "user"
					else if ("," $4 "," !~ ",user,")
						$4 = $4 ",user"
				}
				{ print }
			' /etc/group > "$TMP"
			cat "$TMP" > /etc/group
			rm -f "$TMP"
		fi
		continue
	fi

	printf '%s:x:%s:user\n' "$name" "$gid" >> /etc/group
done

exit 0
