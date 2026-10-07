#!/usr/bin/env bash
# s5-shield installer. Run it with sudo:
#
#     sudo /mnt/Shared/Development/Project/Others/s5-shield/install.sh
#
# It does exactly seven things, and nothing else:
#   1. copies the module source to /usr/src/s5-shield-1.5
#   2. runs the source/hardware self-check first and refuses to continue if the
#      module would not accept this machine's devices (revision 1.0 shipped
#      exactly that bug)
#   3. registers it with DKMS (removing every earlier registration first, so the
#      new source is really rebuilt and no stale version lingers) for the kernel
#      you will boot next; DKMS's pacman hooks then rebuild it automatically on
#      every kernel update
#   4. copies two config files: /etc/modprobe.d/s5-shield.conf (device list) and
#      /etc/modules-load.d/s5-shield.conf (load at boot)
#   5. loads the module now, if it was built for the running kernel - reloading
#      an older copy that is already in the kernel, so what runs is what was
#      just built
#   6. refreshes the shutdown witness if it is installed (that is what produces
#      the record a test shutdown is judged on)
#   7. verifies: module metadata, installed config, and a sha256 of the boot
#      chain before/after, to prove nothing under /boot was touched
#
# Nothing under /boot or /etc/default is modified; Secure Boot is untouched.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG=s5-shield
VER=1.5
TARGET=/usr/src/${PKG}-${VER}
BOOT_BEFORE="$(mktemp -t s5-shield-boot-before.XXXXXX)"
trap 'rm -f "$BOOT_BEFORE"' EXIT

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run me with sudo:  sudo $SRC_DIR/install.sh" >&2
	exit 1
fi

echo "=== 0. before: boot chain fingerprint (must not change) ==="
BOOT_FILES=(/boot/vmlinuz-linux /boot/initramfs-linux.img /boot/amd-ucode.img
	    /boot/loader/loader.conf /boot/loader/entries/arch.conf)
sha256sum "${BOOT_FILES[@]}" 2>/dev/null | tee "$BOOT_BEFORE" || true

echo
echo "=== 1. source -> $TARGET ==="
install -d -m755 "$TARGET"
install -m644 "$SRC_DIR/src/s5_shield.c" "$SRC_DIR/src/Makefile" "$SRC_DIR/src/dkms.conf" "$TARGET/"
ls -la "$TARGET"

echo
echo "=== 1b. source/hardware self-check ==="
if ! "$SRC_DIR/bin/s5-shield-dryrun"; then
	echo "refusing to install: the module as written would not accept this machine's devices" >&2
	exit 1
fi

echo
echo "=== 1c. compiled-logic test ==="
if ! "$SRC_DIR/bin/s5-logictest"; then
	echo "refusing to install: the module's own class predicates mis-handle this machine's devices" >&2
	exit 1
fi

echo
echo "=== 2. DKMS ==="
RUNNING_KVER="$(uname -r)"
if [ -d "/lib/modules/${RUNNING_KVER}/build" ]; then
	KVER="$RUNNING_KVER"
	echo "headers for the running kernel are present: building for $KVER (active immediately)"
else
	KVER="$(ls -1 /usr/lib/modules | sort -V | tail -1)"
	echo "no headers for the running kernel ($RUNNING_KVER); building for the newest installed kernel: $KVER"
	echo "(it becomes active on the next boot)"
fi

# Remove every registered version first: DKMS would otherwise keep building the
# old source tree for future kernels even after this one is installed.
old="$(dkms status 2>/dev/null | sed -n "s|^${PKG}/\([^,]*\),.*|\1|p" | sort -u)"
if [ -n "$old" ]; then
	echo "already registered with DKMS ($(echo $old | tr '\n' ' ')): removing it first so the new source is really rebuilt"
	for v in $old; do
		# If this fails (say the source tree is already gone) the registration
		# is left behind as "broken" in every future `dkms status`, so it is
		# said out loud instead of swallowed.
		dkms remove "${PKG}/${v}" --all || \
			echo "  !! dkms remove ${PKG}/${v} failed; if it stays 'broken', remove it with: sudo dkms remove ${PKG}/${v} --all"
	done
fi

# Keep exactly one source tree for this package: an older /usr/src/s5-shield-*
# left behind is what bin/s5-shield-status would compare against (it reads the
# newest one), and what DKMS would keep rebuilding on every kernel update. Va
# DESPUES de los `dkms remove` a proposito: dkms remove necesita el arbol para
# desregistrar, y borrarlo antes deja la version clavada como "broken" para
# siempre (paso el 2026-10-04, con esta misma linea tres pantallas mas arriba).
for d in /usr/src/"$PKG"-*; do
	[ -d "$d" ] || continue
	[ "$d" = "$TARGET" ] && continue
	rm -rf "$d"
	echo "  removed stale $d"
done

dkms add "${PKG}/${VER}"
dkms build   "${PKG}/${VER}" -k "$KVER"
dkms install "${PKG}/${VER}" -k "$KVER"
echo
dkms status

echo
echo "=== 3. configuration ==="
install -m644 "$SRC_DIR/etc/modprobe.d/s5-shield.conf"     /etc/modprobe.d/
install -m644 "$SRC_DIR/etc/modules-load.d/s5-shield.conf" /etc/modules-load.d/
ls -la /etc/modprobe.d/s5-shield.conf /etc/modules-load.d/s5-shield.conf

echo
echo "=== 4. load now (only possible if it was built for the running kernel) ==="
if [ "$KVER" = "$RUNNING_KVER" ]; then
	if lsmod | grep -qw s5_shield; then
		echo "an older copy is already loaded; unloading it so the reloaded module is the one just built"
		modprobe -r s5_shield || echo "note: could not unload it (it refuses once armed); the new copy takes effect on the next boot"
	fi
	modprobe s5_shield
	lsmod | grep -w s5_shield || true
	echo
	echo "loaded module's own report:"
	dmesg | grep s5-shield | tail -6 || true
else
	echo "built for $KVER, running $RUNNING_KVER -> it loads by itself on the next boot"
fi

echo
echo "=== 4b. refresh the shutdown witness, if it is installed ==="
if [ -f /etc/systemd/system/s5-shutdown-witness.service ] || [ -f /usr/local/bin/s5-shutdown-witness ]; then
	"$SRC_DIR/witness.sh" install
else
	echo "not installed. It is worth it before a test shutdown:"
	echo "    sudo $SRC_DIR/witness.sh install"
fi

echo
echo "=== 5. after: boot chain fingerprint ==="
if sha256sum -c "$BOOT_BEFORE"; then
	echo "OK: /boot is byte-identical. No boot image or boot entry was modified."
else
	echo "WARNING: something under /boot changed - investigate before rebooting." >&2
	exit 1
fi

echo
echo "=== done ==="
echo "Verify any time with:  $SRC_DIR/bin/s5-shield-status"
echo
echo "Then measure it properly (a reboot proves nothing - the shield acts on poweroff only):"
echo "    1. unplug the charger            $SRC_DIR/bin/s5-battery"
echo "    2. sudo systemctl poweroff       (watch the last console lines)"
echo "    3. wait 30-60 min, power on, do not boot in between"
echo "    4. $SRC_DIR/bin/s5-verdict"
echo "Remove everything again with:  sudo $SRC_DIR/uninstall.sh"
echo
echo "On a systemd-boot machine, this repository also carries the firmware power-off path"
echo "for the case the shield cannot settle (~19 W -> 1.32 W measured). Manual arming:"
echo "    $SRC_DIR/bin/s5-halt check"
echo "Automatic detection at shutdown (opt-in; it changes what a poweroff can do):"
echo "    sudo $SRC_DIR/halt.sh install"
