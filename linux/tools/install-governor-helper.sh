#!/bin/sh
# Installs the governor helper (run once with sudo): sudo sh install-governor-helper.sh [user]
# Puts polar-governor in /usr/local/sbin and lets that one user run its "on [schedutil|performance]" and "off" without a password.
# Remove: sudo rm /usr/local/sbin/polar-governor /etc/sudoers.d/zz-polar-governor
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
U=${1:-${SUDO_USER:?give the user name}}; H=$(cd "$(dirname "$0")" && pwd)
install -o root -g root -m 0755 "$H/polar-governor" /usr/local/sbin/polar-governor
T=$(mktemp); G=/usr/local/sbin/polar-governor; printf '%s ALL=(root) NOPASSWD: %s on, %s on schedutil, %s on performance, %s off\n' "$U" $G $G $G $G > "$T"
visudo -cf "$T" >/dev/null && install -o root -g root -m 0440 "$T" /etc/sudoers.d/zz-polar-governor; rm -f "$T"
echo "installed for $U: the Polar launcher now switches the governors while a game runs (NX_GOVERNOR=0 in settings.env turns that off)"
