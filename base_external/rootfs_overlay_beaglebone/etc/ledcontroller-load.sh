#!/bin/sh

module=ledcontroller
device=ledc
mode="644"
set -e

grep -q '^staff:' /etc/group && group=staff || group=wheel

modprobe ${module} "$@"

major=$(awk "\$2==\"$module\" {print \$1}" /proc/devices)
rm -f /dev/${device}
mknod /dev/${device} c $major 0
chgrp $group /dev/${device}
chmod $mode /dev/${device}
