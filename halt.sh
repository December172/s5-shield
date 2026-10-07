#!/usr/bin/env bash
# Install or remove the automatic divert: the shutdown-time decision that routes a
# poweroff through the firmware when the dGPU will not settle.
#
#     sudo /mnt/Shared/Development/Project/Others/s5-shield/halt.sh install
#     sudo /mnt/Shared/Development/Project/Others/s5-shield/halt.sh remove
#
# WHY IT IS SEPARATE FROM install.sh. It changes what a poweroff can do: with it
# installed, a shutdown whose dGPU stays awake becomes a POST plus a poweroff from
# the boot loader instead of a normal poweroff. That is the desired behaviour when
# you know what you are asking for, and a surprise otherwise, so it is opt-in.
#
# It installs both tools: `s5-halt` (arm/status/disarm by hand) and
# `s5-halt-divert` (the automatic decision the unit runs).
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT=/etc/systemd/system/s5-halt-divert.service
UNIT_SRC="$SRC_DIR/systemd/s5-halt-divert.service"
BIN=/usr/local/bin
LOG=/var/log/s5-shield-halt.log

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run me with sudo:  sudo $SRC_DIR/halt.sh ${1:-install}" >&2
	exit 1
fi

case "${1:-install}" in
install)
	"$SRC_DIR/bin/s5-halt" check || {
		echo
		echo "refusing to install: the checks above are what it needs to work." >&2
		exit 1
	}
	install -m755 "$SRC_DIR/bin/s5-halt"        "$BIN/s5-halt"
	install -m755 "$SRC_DIR/bin/s5-halt-divert" "$BIN/s5-halt-divert"
	install -m644 "$UNIT_SRC" "$UNIT"
	systemctl daemon-reload
	systemctl enable --now s5-halt-divert.service
	cat <<EOF

installed and enabled.

What happens now: on every shutdown, ${BIN##*/} waits up to 20 s for the dGPU to
reach D3cold. If it gets there - the usual case - nothing changes. If it does not,
the tool arms the boot loader's one-shot and reboots, and the firmware powers the
machine off with no kernel in that boot (you will see a POST; that is expected).

Test it on purpose by forcing the dGPU awake first:

    echo on | sudo tee /sys/bus/pci/devices/0000:01:00.0/power/control
    sudo systemctl poweroff

and judge the result from the next boot with:  $SRC_DIR/bin/s5-evidence

Disarm it for one shutdown without uninstalling:

    sudo touch /var/lib/s5-shield/halt-divert-disabled

Watch what it decided, after any shutdown:

    journalctl -b -1 -u s5-halt-divert.service -o cat
    cat $LOG

Remove it again with:  sudo $SRC_DIR/halt.sh remove
EOF
	;;
remove)
	systemctl disable --now s5-halt-divert.service 2>/dev/null || true
	rm -fv "$UNIT" "$BIN/s5-halt" "$BIN/s5-halt-divert"
	systemctl daemon-reload
	echo "removed (the brake file, if you created one, is left: /var/lib/s5-shield/halt-divert-disabled)"
	;;
*)
	echo "usage: sudo $0 {install|remove}" >&2
	exit 2
	;;
esac
