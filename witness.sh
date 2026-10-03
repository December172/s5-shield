#!/usr/bin/env bash
# Install or remove the shutdown-time witness (the diagnostic that records which
# D-state the dGPU was really in on the way into S5).
#
#     sudo ~/Tools/s5-shield/witness.sh install
#     sudo ~/Tools/s5-shield/witness.sh remove
#
# The witness does not change any power behaviour. It only reads sysfs late in
# the shutdown and appends the result to the journal and to
# /var/log/s5-shield-witness.log. It is meant to be installed BEFORE a test
# shutdown, so that if the chassis still gets warm, the journal says why.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT=/etc/systemd/system/s5-shutdown-witness.service
BIN=/usr/local/bin/s5-shutdown-witness
LOG=/var/log/s5-shield-witness.log

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run me with sudo:  sudo $SRC_DIR/witness.sh ${1:-install}" >&2
	exit 1
fi

case "${1:-install}" in
install)
	install -m755 "$SRC_DIR/bin/s5-shutdown-witness" "$BIN"
	install -m644 "$SRC_DIR/systemd/s5-shutdown-witness.service" "$UNIT"
	systemctl daemon-reload
	systemctl enable --now s5-shutdown-witness.service
	echo
	echo "installed and enabled. It records a short boot snapshot now, and does the"
	echo "real work on the way into S5. After your next poweroff, read the result with:"
	echo "    journalctl -b -1 -u s5-shutdown-witness.service -o cat"
	echo "    cat $LOG"
	echo "then judge it with:  ~/Tools/s5-shield/bin/s5-verdict"
	echo
	echo "remove it again with:  sudo $SRC_DIR/witness.sh remove"
	;;
remove)
	systemctl disable --now s5-shutdown-witness.service 2>/dev/null || true
	rm -fv "$UNIT" "$BIN" "$LOG"
	systemctl daemon-reload
	echo "witness removed"
	;;
*)
	echo "usage: sudo $0 {install|remove}" >&2
	exit 2
	;;
esac
