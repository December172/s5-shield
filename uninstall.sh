#!/usr/bin/env bash
# s5-shield uninstaller. Run it with sudo:
#
#     sudo /mnt/Shared/Development/Project/Others/s5-shield/uninstall.sh
#
# Removes everything the installer put on the machine: the loaded module, every
# DKMS registration and its built module, the two config files, the source trees,
# the optional shutdown witness, and the opt-in firmware power-off path (the
# divert unit plus the two tools in /usr/local/bin). It touches nothing else - no
# boot image, no boot entry, no kernel, no firmware setting. After this the machine
# behaves exactly as it did before the installer ran.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG=s5-shield

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run me with sudo:  sudo $0" >&2
	exit 1
fi

echo "=== 1. unload the module if it is loaded and was not armed ==="
if lsmod | grep -qw s5_shield; then
	if modprobe -r s5_shield 2>/dev/null; then
		echo "unloaded"
	else
		echo "could not unload (it may already be armed for this poweroff)."
		echo "That is harmless: the shield lives in RAM and disappears on reboot."
	fi
else
	echo "not loaded"
fi

echo
echo "=== 2. shutdown witness (only if it is installed) ==="
if [ -f /etc/systemd/system/s5-shutdown-witness.service ] || [ -f /usr/local/bin/s5-shutdown-witness ]; then
	"$SRC_DIR/witness.sh" remove
else
	echo "not installed"
fi

echo
echo "=== 2b. firmware power-off path (only if it is installed) ==="
if [ -f /etc/systemd/system/s5-halt-divert.service ] || [ -f /usr/local/bin/s5-halt-divert ]; then
	"$SRC_DIR/halt.sh" remove
else
	echo "not installed"
fi

echo
echo "=== 3. DKMS (every version that is registered) ==="
old="$(dkms status 2>/dev/null | sed -n "s|^${PKG}/\([^,]*\),.*|\1|p" | sort -u)"
if [ -n "$old" ]; then
	for v in $old; do
		dkms remove "${PKG}/${v}" --all
	done
else
	echo "not registered"
fi

echo
echo "=== 4. configuration files ==="
rm -fv /etc/modprobe.d/s5-shield.conf /etc/modules-load.d/s5-shield.conf

echo
echo "=== 5. source trees ==="
rm -rfv /usr/src/${PKG}-*/

echo
echo "=== done: the machine is back to its previous behaviour ==="
echo "(/boot was never touched, so there is nothing to restore there)"
