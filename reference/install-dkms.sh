#!/usr/bin/env bash
# install-dkms.sh: the OTHER solution in this story, packaged for DKMS.
#
# WHAT. Upstream's fix (AnxoCalvo/s5-poweroff-fix) is a small module, `s5_pmrt_arm`,
# plus a policy that waits for the dGPU before shielding it. Its packaging is Fedora
# only: rpmbuild + akmods, which recompiles the module on every kernel update.
#
# WHY IT IS HERE. Outside Fedora there is no akmods, so that property is lost and the
# user is left rebuilding by hand after every kernel - the failure mode its own
# REBUILDING.md warns about (a stale `.ko` that `insmod` rejects, and a mitigation
# that dies silently). DKMS gives the property back from the same source. Since this
# repository is where systemd-boot users are pointed, it also carries the packaging
# that makes the reference solution usable on the distributions its author does not
# ship for.
#
# Usage:
#   sudo reference/install-dkms.sh [kernel]        (default: the running one)
#
# Point it at a checkout instead of cloning upstream:
#   sudo S5_PMRT_SRC=/path/to/s5-poweroff-fix reference/install-dkms.sh
#
# IT DOES NOT LOAD THE MODULE. Upstream loads it from its `99y` shutdown hook, and
# here it is only made available. **Do not load it while s5_shield is in use**: both
# override the `.shutdown` of the same drivers, and the result cannot be read.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
PKG=s5-pmrt-arm
VER=1.0
TARGET=/usr/src/$PKG-$VER
KVER=${1:-$(uname -r)}
CACHE=${S5_PMRT_CACHE:-/var/cache/s5-shield/upstream}
SRC=${S5_PMRT_SRC:-}

[ "$(id -u)" = 0 ] || { echo "this writes to /usr/src and calls dkms: use sudo" >&2; exit 1; }
command -v dkms >/dev/null || { echo "dkms is missing (Arch: pacman -S dkms; Debian: apt install dkms)" >&2; exit 1; }
[ -d "/lib/modules/$KVER/build" ] || { echo "the kernel headers for $KVER are missing" >&2; exit 1; }

if [ -z "$SRC" ]; then
	if [ -d "$CACHE/kmod/$PKG-$VER" ]; then
		SRC=$CACHE
		echo "using the checkout already in $CACHE"
	else
		command -v git >/dev/null || { echo "no git, and no S5_PMRT_SRC: point it at a checkout" >&2; exit 1; }
		echo "cloning upstream into $CACHE"
		mkdir -p "$(dirname "$CACHE")"
		git clone --depth 1 https://github.com/AnxoCalvo/s5-poweroff-fix "$CACHE" || {
			echo "the clone failed; fetch it yourself and pass S5_PMRT_SRC=/path/to/it" >&2; exit 1; }
		SRC=$CACHE
	fi
fi

[ -f "$SRC/kmod/$PKG-$VER/kernel/s5_pmrt_arm.c" ] || {
	echo "I cannot find $SRC/kmod/$PKG-$VER/kernel/s5_pmrt_arm.c" >&2; exit 1; }

echo "== 1. stage the source in $TARGET =="
# The whole directory, kernel/ included: that is the layout this dkms.conf expects,
# and the directory is also the spec's Source0 upstream. `dkms.conf` is installed
# from HERE, not from the checkout, so the two fixes below are guaranteed even on a
# tree that has no dkms.conf at all (the packaging was never merged upstream).
rm -rf "$TARGET"
install -d "$TARGET"
cp -a "$SRC/kmod/$PKG-$VER/." "$TARGET/"
install -m644 "$HERE/dkms.conf" "$TARGET/dkms.conf"

echo
echo "== 2. remove previous versions of $PKG =="
# `dkms add` fails if the version is already registered, and a version left 'broken'
# stays that way in `dkms status` forever, so anything that is not this one goes.
for v in $(dkms status "$PKG" 2>/dev/null | sed -n "s|^$PKG/\([^,]*\),.*|\1|p" | sort -u); do
	echo "   removing $PKG/$v"
	dkms remove "$PKG/$v" --all >/dev/null 2>&1 || \
		echo "   !! could not remove $PKG/$v; if it stays 'broken': dkms remove $PKG/$v --all"
done

echo
echo "== 3. add / build / install for $KVER =="
dkms status "$PKG/$VER" 2>/dev/null | grep -q "^$PKG/$VER" || dkms add "$PKG/$VER"
dkms build   "$PKG/$VER" -k "$KVER"
dkms install "$PKG/$VER" -k "$KVER"

echo
dkms status "$PKG/$VER" || true
echo
echo "check it:   modinfo s5_pmrt_arm | head -3"
echo "the .ko is in /usr/lib/modules/$KVER/updates/dkms/ (dkms overrides"
echo "DEST_MODULE_LOCATION per distribution). Note that upstream's own install.sh looks"
echo "only in extra/s5-pmrt-arm/ and will report the module as missing - see README.md"
echo "in this directory for the one-line change that fixes that."
echo
echo "Reminder: this only makes the module AVAILABLE. Do not modprobe it while"
echo "s5_shield is loaded; remove one before loading the other."
