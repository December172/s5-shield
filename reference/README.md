# `reference/` — the other solution in this story, packaged for DKMS

Upstream's fix ([AnxoCalvo/s5-poweroff-fix](https://github.com/AnxoCalvo/s5-poweroff-fix)) is a small
module, `s5_pmrt_arm`, plus a policy that waits for the dGPU before shielding it. That is the
reference solution this repository's own module grew out of, and on Fedora it is the complete one.

It ships as an **akmod**, which means Fedora in practice: `rpmbuild` + `akmods` recompile the module
on every kernel update, and nothing else gets that for free. This directory carries the missing half —
the same source, packaged for **DKMS**, which is the equivalent mechanism everywhere else.

```bash
sudo reference/install-dkms.sh                 # clones upstream into /var/cache/s5-shield/upstream
sudo S5_PMRT_SRC=/path/to/s5-poweroff-fix reference/install-dkms.sh   # or use a checkout you have
```

**It does not load the module.** Upstream loads it from its `99y` shutdown hook. Do not `modprobe` it
while `s5_shield` is in use: both override the `.shutdown` of the same drivers and the result cannot be
read. Switch from one to the other, not both at once.

## What the packaging needed, and why it is worth writing down

Both of these were found by running it, not by reading it.

1. **`BUILT_MODULE_LOCATION[0]="kernel/"`.** The first end-to-end run failed with

       Error! Build of s5_pmrt_arm.ko failed for: 7.2.9-arch1-1 (x86_64)

   while the build log said `exit code: 0` and the `.ko` sat in `build/kernel/`. The compile was fine;
   dkms looks for the built module in the root of the build tree unless told otherwise, and the
   Makefile here lives one directory down.

2. **No `CLEAN=`.** dkms 3.x moved it to its obsolete list and printed

       Deprecated feature: CLEAN (/usr/src/s5-pmrt-arm-1.0/dkms.conf)

   on every build. dkms manages the build tree itself now, so the line bought nothing and taught the
   reader to ignore warnings.

## Where the module ends up

`DEST_MODULE_LOCATION` is mandatory in dkms 3.x, but dkms then overrides it per distribution
(`override_dest_module_location`):

| distribution | where the module is installed |
|---|---|
| Fedora / RHEL / CentOS | `/extra` |
| openSUSE / SLES | `/updates` |
| Debian / Ubuntu / **Arch** | `/updates/dkms` |

On Arch and Debian that is `/usr/lib/modules/<kernel>/updates/dkms/`, which is what our installer
reports.

**One line upstream does not have.** Its `install.sh` checks only
`/lib/modules/$(uname -r)/extra/s5-pmrt-arm/`, the akmods location, so a perfectly valid DKMS install
makes it print `FALTA EL MODULO DEL KERNEL` - a warning that fires when nothing is wrong. The fix is a
second glob:

```sh
ko_dkms=$(ls /lib/modules/"$(uname -r)"/updates/dkms/s5_pmrt_arm.ko* 2>/dev/null | head -1)
```

That change was offered upstream and the pull request was closed (the maintainer's four reasons are
worth reading; one of them was a real regression of ours in a different file). The packaging is kept
here rather than lost, and anyone who wants to file it upstream has it ready.

## Two things to know before touching the module's source

* **Its log lines are a contract.** Upstream's `s5-mitigacion-check` parses fragments of what the
  module prints: `s5-pmrt-arm: ===== ... devs=`, `FIN ... disable= ... shutdown_anulados=`, and
  `.shutdown de '<driver>' ANULADO|SE RESPETA`. Translate them piecemeal and the audit does not
  crash - it silently stops matching, which is worse. The comments and `MODULE_DESCRIPTION` /
  `MODULE_PARM_DESC` are safe; the fragments above are not.
* **Installing it does not mean it is running.** DKMS only builds and installs; if you want the
  reference policy to drive it, install upstream's own hooks (`install.sh` there) rather than
  `modprobe`-ing it by hand.

## Tested

On Arch against `7.2.9-arch1-1`, with this `dkms.conf`:

```
$ dkms status
s5-pmrt-arm/1.0, 7.2.9-arch1-1, x86_64: installed

$ modinfo s5_pmrt_arm
filename:       /lib/modules/7.2.9-arch1-1/updates/dkms/s5_pmrt_arm.ko.zst
description:    Step 1: shields the dGPU subtree against pci_device_shutdown()
license:        GPL
srcversion:     FEE41612B3ACC3473ACA2A6
```

`dkms install` also **signed** the module itself (`Signing module ...`), so on a Secure Boot machine
with DKMS MOK keys enrolled it is loadable without an extra step. Not tested on Fedora: this
directory does not touch the akmods path.
