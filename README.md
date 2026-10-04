# s5-shield — keep the discrete GPU in D3cold across poweroff

Machine: **HP OMEN Gaming Laptop 16-ap0xxx** (`8E35`), BIOS **F.13**, AMD Ryzen 9 8945HX
(iGPU `amdgpu`) + NVIDIA GB206M / RTX 5060 Max-Q (`nvidia`, open 615.71.09), Arch Linux,
kernel **7.2.8-arch1-2**, ACPI S-states `S0 S4 S5`.

Status: **installed and measured working** on this machine — see *Result*. The repository packages
the module as DKMS `s5-shield/1.4`; the machine's registration moves to it the next time
`install.sh` runs.

## The problem

Screen dark, machine silent, chassis warm near the GPU, battery flat by morning — an "S5" that
draws ≈20 W. The cause is in your kernel's source (v7.2.8 `drivers/pci/pci-driver.c:557-576`):

```c
static void pci_device_shutdown(struct device *dev)
{
	...
	pm_runtime_resume(dev);        /* unconditional, even for a poweroff */

	if (drv && drv->shutdown)
		drv->shutdown(pci_dev);
}
```

`device_shutdown()` walks every device at poweroff, so the dGPU — asleep since the session
released it — is woken on the way out. Awake, the platform never cuts its rail, and the machine
sits in a fake S5 drawing ~20 W. Windows reaches the real S5 on the same hardware, which is why
booting Windows appears to "finish" the shutdown.

* Independent measurement on the same model: [evidence log](https://github.com/AnxoCalvo/s5-poweroff-fix/blob/main/docs/EVIDENCE.md)
  — GPU arriving in `D0` = 19.85 W, in `D3cold` = 0.2–2 W.
* Same symptom, no fix found: [CachyOS forum thread](https://discuss.cachyos.org/t/technical-issue-incomplete-shutdown-on-hp-omen-16-ap0038ns/26236).
* The upstream patch that would fix it — [PCI: Avoid runtime resuming devices if system is shutting down](https://lkml.iu.edu/hypermail/linux/kernel/2312.1/07199.html)
  (Mario Limonciello, Dec 2023) — was never merged.

Not helping on this model (measured by others, not guessed): `acpi=force`, `acpi=noirq`,
`acpi_osi=…`, `reboot=pci|efi`, `acpi_sleep=s5`, `acpi=off`, disabling nvidia or using nouveau,
`nvidia-powerd`, and a userspace shutdown hook that forces `D3cold` (the kernel wakes the device
again *after* it). A patched kernel would work but means touching signed boot images.

## The fix

One out-of-tree module, `s5_shield` (503 lines of code in an 883-line file that is mostly the
reasoning, GPL-2.0, `src/s5_shield.c`). It registers a **reboot notifier**, which
`kernel_power_off()` runs *before* `device_shutdown()` (`kernel/reboot.c:303-310`), so by the time
the PCI core walks the device list it has already:

0. **recorded why the dGPU is (or is not) asleep** — the runtime PM accounting (`rpm=`, `use=`,
   `child=`, `dis=`) and, when it is awake, the blocking condition named in the runtime PM core's
   own vocabulary (`rpm_blocker()` reads the same conditions `rpm_check_suspend_allowed()` uses:
   `-EACCES` disabled, `-EAGAIN` a held reference, `-EBUSY` an active child);
1. **disabled runtime PM** on the listed devices (`__pm_runtime_disable()`), so the
   `pm_runtime_resume()` above bounces with `-EACCES` instead of powering the GPU up
   (`drivers/base/power/runtime.c:798-808`);
2. **nulled `drv->shutdown`** for the drivers named in `noshut` (`nvidia`, `snd_hda_intel`), so no
   driver teardown runs MMIO against an unpowered device.

Steps 1 and 2 must go together: 1 alone leaves `nv_pci_shutdown()` executing against a device in
`D3cold`, and that bus-locks this machine.

This is the same two-halves method published for this model by Anxo Calvo (see *Credits*), with two
differences: it arms itself from the kernel's reboot notifier instead of from a shutdown hook, and
it measures what it does instead of assuming it.

**The wait of revision 1.3 is gone — off by default, `wait_ms=0`.** It was meant to let the dGPU
fall asleep before the shield pinned it awake, and it cannot do that: `pm_request_idle()` only
*asks* the runtime PM core, which refuses with `-EAGAIN` while any driver or client holds a
reference and with `-EBUSY` while a device below is active (`rpm_check_suspend_allowed()`) — and a
device that *can* suspend has already been idle-notified by the core when its last reference was
dropped. When the request is accepted, `rpm_suspend(RPM_AUTO)` waits out the device's autosuspend
delay instead of suspending at once: there is no near-instant version of this to be had from inside
the kernel. Measured here (2026-10-03, 15:13 poweroff): the 5000 ms wait ran out with the dGPU
still in `D0`, the module armed anyway, and the 50-minute S5 that followed still cost ~1 W. The
saving comes from **1+2** — the kernel never resumes the GPU and `nv_pci_shutdown()` never runs.
If `wait_ms` is set anyway, phase 1 asks every listed device to go idle once and only phase 2 spends
the budget polling, so the result no longer depends on the order of `devs=`.

**What decides the rail is now measured, not inferred.** Everything above runs *before*
`device_shutdown()`; the references that keep the dGPU awake are released *inside* it. So a
`SYS_OFF_MODE_POWER_OFF_PREPARE` handler (`s5_shield_observe_final()`) prints a `FINAL` line per
device after the walk and before the firmware poweroff — the last observable moment
(`kernel_power_off()`: notifier + `device_shutdown()` → power-off-prepare → `syscore_shutdown()` →
`machine_power_off()`). Read it as: dGPU `D3cold` with the bridge in `D3hot`/`D3cold` ⇒ the rail was
released before the firmware took over, i.e. the shield worked by *not resuming* it, and the `D0`
seen at arming time was not the deciding factor; anything still in `D0` ⇒ whatever held it awake
(the `use=`/`child=` fields say which) was released too late or never, and *that* is the case a
shutdown-time fallback has to cover — not a longer wait.

The expensive case is still a GPU that is **busy** at poweroff — a CUDA job, an external display,
PRIME offload — which the shield would freeze busy for ~19 W the whole night. Upstream covers that
with a 90 s wait plus a GRUB `halt` fallback; this machine, with systemd-boot, has no such fallback
yet.

Safety rails: nothing happens on reboot/halt or while the system runs; `devs=` is mandatory, and
every address is resolved and class-checked (display / audio / PCI bridge only) both at load time
and again at poweroff; `amdgpu`, `nvme`, `ahci`, `xhci_hcd`, networking and the rest are refused
outright; `pcieport` is never added to `noshut` (it governs every port in the machine); a listed
bridge must really be an ancestor of a listed device; if anything is off, the module refuses to
load and you simply get today's behaviour.

## Install / uninstall (root)

```bash
sudo /mnt/Shared/Development/Project/Others/s5-shield/install.sh     # checks, DKMS build, configs, live reload, /boot fingerprint
sudo /mnt/Shared/Development/Project/Others/s5-shield/uninstall.sh   # back to the previous behaviour, nothing left behind
```

`install.sh` runs the two self-checks in `bin/` first and **refuses to install** if the module
would not accept this machine's devices. DKMS then rebuilds it on every kernel update.
`/etc/modprobe.d/s5-shield.conf` holds the device list, the driver list and `wait_ms`.

## Verify and measure

```bash
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-shield-status    # loaded? installed source == this revision? device states
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-shield-dryrun    # what the module would do, read out of the C source
dmesg | grep s5-shield                    # "ready: 3 target(s), …, wait_ms=0, FINAL observer=on"
```

A **reboot proves nothing** (the shield acts on poweroff only), and uptime drowns the effect: this
laptop draws 24–78 W awake, i.e. 4–8% of the battery per five minutes. Measure the off window and
nothing else:

```bash
# 1. charge it, then UNPLUG the charger (AC masks everything)
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-battery
# 2. sudo systemctl poweroff        (a real poweroff, not a reboot)
# 3. wait 30-60 min, power on, then:
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-verdict          # watts across the OFF window
cat /var/log/s5-shield-witness.log        # what state the dGPU was in, and when
```

The optional **shutdown witness** (`sudo /mnt/Shared/Development/Project/Others/s5-shield/witness.sh install`, refreshed
automatically by `install.sh`) records the dGPU, its root port and the port's ACPI state with
timestamps on the way into S5, plus the battery at both ends of the off window. It only reads
sysfs — no `lspci`, no `nvidia-smi`, nothing that could wake the device being measured.

### The silent-failure alarm

The failure mode of this whole fix is silence: the shutdown looks perfect whether or not the
shield did anything. So the witness also runs a **two-way self-check on every boot** (idea taken
from upstream's `s5-mitigacion-check`) and writes it into the same record:

```
check: OK - FORWARD: shield loaded, accepted its device list, armed for the next poweroff
check: OK - BACKWARD: last poweroff drew 0.31 W over 1.20 h (the boot itself is inside that)
```

* **forward** — is `s5_shield` loaded and did it report `ready` in this boot's kernel log? If not,
  the *next* poweroff will burn ~20 W again.
* **backward** — was the *last* one shielded? That is the off-window arithmetic above, judged only
  when it means something: early in the boot, charger unplugged at both ends, window ≥ 15 min.
  Anything else says `not judged` and why, instead of inventing a number.

If either fails, the check also writes `/var/lib/s5-shield/check-failed` and shouts on the
terminals (`wall`); `bin/s5-shield-status` reports it under **boot self-check**. The witness log is
capped at 256 KB (keeping the tail, which always contains the last shutdown record) so it cannot
grow forever — the same concern upstream solves with `logrotate`, without the extra packaging.

The kernel prints its own account as the machine goes down, visible even with `quiet loglevel=3`.
Every line below the `FINAL` marker is the module's own output; the last four come from the
power-off-prepare observer. (`wait_ms=0` in revision 1.4, so the two wait lines of the 15:13
transcript are gone.)

```
s5-shield: poweroff path, 3 device(s) listed
s5-shield: wait_ms=0, not waiting: 0000:01:00.0 is D0, usage_count > 0: a driver or client still holds a reference (-EAGAIN) [rpm=active use=1 child=0 dis=0 auto=yes], ...
s5-shield: 0000:01:00.0 state=D0 acpi=D0 driver=nvidia rpm=active use=1 child=0 dis=0 auto=yes
s5-shield: 0000:01:00.0 still in D0, cannot suspend after this: this poweroff can still be warm (README: Reading the console)
s5-shield: 0000:01:00.0 .shutdown of 'nvidia' nulled
s5-shield: 0000:01:00.1 state=D3hot acpi=D3hot driver=snd_hda_intel rpm=suspended use=0 child=0 dis=0 auto=yes
s5-shield: 0000:01:00.1 .shutdown of 'snd_hda_intel' nulled
s5-shield: 0000:00:01.1 state=D0 acpi=D0 driver=pcieport rpm=active use=1 child=1 dis=0 auto=yes parent=suspended
s5-shield: 0000:00:01.1 bridge: .shutdown of 'pcieport' left in place on purpose
s5-shield: 0000:00:01.1 still in D0, cannot suspend after this: this poweroff can still be warm (README: Reading the console)
s5-shield: done: runtime PM off on 3, .shutdown nulled on 2, skipped 0
s5-shield: FINAL state, after device_shutdown() and before the firmware poweroff
s5-shield: FINAL 0000:01:00.0 pci=D3cold acpi=D3cold driver=nvidia rpm=suspended use=0 child=0 dis=1 auto=no parent=suspended
s5-shield: FINAL 0000:01:00.1 pci=D3cold acpi=D3cold driver=snd_hda_intel rpm=suspended use=0 child=0 dis=1 auto=no
s5-shield: FINAL 0000:00:01.1 pci=D3hot acpi=D3hot driver=pcieport rpm=suspended use=0 child=0 dis=1 auto=no parent=suspended
```

The shape to expect on a healthy poweroff is the one above: **`D0` at arming time, `D3cold`/
`D3hot` in the `FINAL` lines.** The `can still be warm` warnings are the module being accurate
rather than reassuring — the dGPU and its port were awake when it armed, so all it could do was
keep the kernel and the driver away from them, and the drop happened later, inside the device walk.
The two `use=1`/`child=1` fields at arming time are the exact reason the wait of revision 1.3 could
never have changed that: the root port cannot suspend while the GPU under it is active. (The
`FINAL` values above are what this revision is expected to print; the transcript is filled in from
the first instrumented poweroff with 1.4 and until then the arming half is the measured part.)

## Reading the console

Two things tell you everything after a poweroff, and both appear on the console during shutdown and
in `journalctl -b -1 -k`. The arming lines (revision 1.4) say *why* each device is where it is; the
`FINAL` lines say where the tree actually ended up, after `device_shutdown()` and before the
firmware poweroff — the moment that decides the rail.

| what you see | what it means | what to do |
|---|---|---|
| arming `state=D0 … use=1 child=0`, then `FINAL … pci=D3cold` | the dGPU was awake when the shield armed, and the device walk released it: the rail was cut before the firmware took over. This is the **healthy** case on this laptop, and it is why the wait was never the mechanism. | nothing — this is the fix working |
| `FINAL … pci=D0` on the GPU, or the bridge still `pci=D0` | whatever held it awake (`use=` a reference, `child=` an active device below) was released too late or never, so the rail is likely still on. | find the holder from the two counters; this is the case that needs a shutdown-time fallback, not a longer wait (upstream's 90 s + GRUB `halt`; this machine has none yet) |
| no `FINAL` line at all | the observer did not register — check the boot line `ready: … FINAL observer=on`, or that the kernel reached power-off-prepare. | nothing: the shield works without it, you only lose the measurement |

The fields: `rpm=` is the device's runtime PM status, `use=` is how many references hold it awake
(`-EAGAIN` when the wait asks it to suspend), `child=` is how many devices below it are still active
(`-EBUSY`; a root port with `child=1` is the normal state while its GPU is awake), `dis=` is the
disable depth this module itself raises when it arms, `auto=` is the `power/control` setting, and
`acpi=` is the ACPI power state of the same device — the platform-side half of "is the rail off".
The arming-time `D0` on its own is **not** an alarm: only the `FINAL` line decides.

The `FINAL` lines are printed immediately before the firmware is asked to power off, so they are on
screen only for as long as the S5 transition takes. To photograph them at leisure, load the module
with the diagnostic hold — it changes nothing else, and the next reboot drops it again:

```bash
sudo modprobe -r s5_shield && sudo modprobe s5_shield final_hold_ms=8000
# ... now power off and photograph the screen; nothing to undo afterwards
```

## Result (2026-10-03)

First instrumented poweroff: 11:51:04 → 12:41:07, **50 minutes off**, on battery with the charger
unplugged the whole time. This is revision 1.3 (the 5 s wait still enabled); revision 1.4 keeps the
same two halves and turns that wait off, so the number is expected to hold — the confirming run,
with the `FINAL` lines, is the next poweroff.

* The witness ran (that is itself a fix — see *History*) and recorded the dGPU **still in `D0`
  after 6 s with no holders**: the nvidia driver does not runtime-suspend it on its own once its
  clients are gone. The module's own 5 s wait ends the same way — a photo of the 15:13 poweroff
  shows `still D0 after 5000 ms, giving up on the wait` — so step 0 is *not* what makes these
  poweroffs cheap. **(a)+(b) are:** the kernel never resumes the GPU and `nv_pci_shutdown()` never
  runs. An idle `D0` GPU is nearly free; the ~19-20 W case is a *busy* one.
* Battery at the last moment before poweroff **64.743 Wh (82.8%)**; first sample after boot
  **80%** — 2.2 Wh, of which the three minutes of uptime in between are worth 1.2–2.9 Wh. So the
  50 minutes the machine was actually off cost **at most ~1 Wh (≈1 W)**. The "≈16.7 Wh the old
  behaviour produced over the same window" is arithmetic from the published ~20 W for this model
  (20 W × 0.84 h), not a measurement taken on this unit — this machine's pre-fix drain was never
  measured here, only its symptom (warm chassis, flat battery).
* Chassis cold — the symptom this started from, gone.

## Limits

* If the dGPU does not reach `D3cold` before the shield arms, the module says so — with the reason
  — and that poweroff is no worse than before: the rail was never released, so there was nothing to
  preserve. The remaining
  lever would be `pcie_port_pm=force` on the kernel command line, which is out of scope here.
* The wait is off (`wait_ms=0`): on this machine it cost 5 s per poweroff, timed out every time, and
  the `FINAL` line is what now says whether the rail actually dropped. Re-enable it with
  `wait_ms=5000` only to re-run that experiment.
* The shield is not undone by `modprobe -r`: once armed, the next poweroff is committed. The worst
  case of this class of change is a hang at poweroff — hold the power button ~10 s; the
  filesystems are already unmounted at that point. It cannot hang a reboot: it only arms for
  `SYS_POWER_OFF`.
* If a future kernel changes `struct pci_driver` or the notifier ordering, DKMS still builds it;
  the worst case is a load failure, i.e. the old behaviour.

### History

Revisions 1.0 and 1.1 both shipped a module that silently refused to load its own device list (a
wrong audio class macro, then a `class >> 16` shift against 16-bit macros) — so the fix was inert
and nothing said so. 1.2 fixed the decoding; 1.3 added the wait (step 0); 1.4 turns that wait off
and replaces it with measurement: the arming-time blocking reason, and a `FINAL` line at the moment
of no return. All three bugs were caught by review, not by testing, which is why
`bin/s5-shield-dryrun` and `bin/s5-logictest` now read the accept-list, the shift **and the
blocker logic** out of the C source and check them against the live hardware instead of mirroring
them. The witness unit had a fourth such bug: with `DefaultDependencies=no` and only
`Before=shutdown.target`, systemd never stopped it, so it never recorded anything.

## Files

| path | what it is |
|---|---|
| `src/s5_shield.c` | the whole fix: one reboot notifier plus two diagnostics, 503 lines of code, GPL-2.0 |
| `src/{Makefile,dkms.conf}` | build and DKMS packaging for it |
| `etc/modprobe.d/s5-shield.conf` | the device list (`devs=`), the driver list (`noshut=`), `wait_ms=` || `etc/modules-load.d/s5-shield.conf` | loads the module at boot |
| `install.sh`, `uninstall.sh` | the only two things you have to run |
| `witness.sh` | installs/removes the optional shutdown witness |
| `systemd/s5-shutdown-witness.service` | the witness unit (boot record + shutdown record) |
| `bin/s5-shutdown-witness` | the witness itself: boot self-check + shutdown timeline, reads sysfs only |
| `bin/s5-verdict` | pairs the shutdown and boot records, prints the watts across the off window |
| `bin/s5-shield-status` | module loaded? installed source this revision? self-check result, device states |
| `bin/s5-battery` | one-line battery/AC snapshot for a before/after measurement |
| `bin/s5-shield-dryrun`, `bin/s5-logictest` | the two self-checks `install.sh` refuses to skip (the second also compiles and runs the blocker logic) |

Everything here is **GPL-2.0-only** (`LICENSE`); the module declares it with an SPDX tag.
Nothing outside `/usr/src/s5-shield-1.4`, `/etc/modprobe.d`, `/etc/modules-load.d`,
`/lib/modules/<kver>/updates/dkms` and the optional witness unit is touched.

## Credits

* Diagnosis, the two-halves method and the evidence base for this exact model: **Anxo Calvo**,
  [s5-poweroff-fix](https://github.com/AnxoCalvo/s5-poweroff-fix) — also the source of the
  `pcieport` exception, the `nvidia`/`snd_hda_intel` whitelist, the kexec explanation, the
  boot self-check idea, the charge-based battery fallback and the log-rotation rationale;
  upstream analysis and the never-merged fix: **Mario Limonciello** and the LKML thread above.
* The module, the tooling and this document were written by the **deepseek-flash** coding agent
  (DeepSeek Harness) in session with december172, 2026-10-03 — including finding two bugs in its
  own earlier revisions, the third in the witness unit, and the instrumented test that settled
  whether the fix works.
