# s5-shield — keep the discrete GPU in D3cold across poweroff

Machine: **HP OMEN Gaming Laptop 16-ap0xxx** (`8E35`), BIOS **F.13**, AMD Ryzen 9 8945HX
(iGPU `amdgpu`) + NVIDIA GB206M / RTX 5060 Max-Q (`nvidia`, open 615.71.09), Arch Linux,
kernel **7.2.8-arch1-2**, ACPI S-states `S0 S4 S5`.

Status: **installed and measured working** on this machine — see *Result*. The repository packages
the module as DKMS `s5-shield/1.5`; the machine's registration moves to it the next time
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

One out-of-tree module, `s5_shield` (503 lines of code in a 910-line file that is mostly the
reasoning, GPL-2.0, `src/s5_shield.c`). It registers a **reboot notifier**, which
`kernel_power_off()` runs *before* `device_shutdown()` (`kernel/reboot.c:303-310`), so by the time
the PCI core walks the device list it has already:

0. **waited for the dGPU to settle, and recorded why it is (or is not) asleep** — it asks each
   listed device to go idle and waits up to `wait_ms` (default **20000 ms**) for the display device
   to reach `D3cold` with its bridge out of `D0`, and prints the runtime PM accounting (`rpm=`,
   `use=`, `child=`, `dis=`) plus, when the device is awake, the blocking condition named in the
   runtime PM core's own vocabulary (`rpm_blocker()` reads the same conditions
   `rpm_check_suspend_allowed()` uses: `-EACCES` disabled, `-EAGAIN` a held reference, `-EBUSY` an
   active child). The waiting half is load-bearing and was measured to be: with the same module
   binary, arming a subtree that is still awake with no wait left the rail on (18.51 W), and 20 s of
   waiting cut it — 0.43 W over 11 h, and in the baseline's own 2.5 h window **0.42 W** against the
   unshielded 20.33 W. See *Result*;
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

**The wait is the mechanism, and it is on by default (`wait_ms=20000`).** Revision 1.3 added it;
revision 1.4 retired it on the theory that the dGPU falls asleep later anyway, inside
`device_shutdown()`. That theory was wrong, and the diagnostics 1.4 added are what proved it. Same
module binary (`srcversion 5ABD41F6E01E06E371E5D2F`), same arm-time state (dGPU `D0`, root port
`D0`, audio `D3hot`, no holders), one parameter:

| `wait_ms` | window | off draw | ledger |
|---|---|---|---|
| `0` (the 1.4 default) | 0.34 h | **18.51 W** | `FAIL` — the boot check: *the rail was NOT cut* |
| `20000` (the 1.5 default) | 11.12 h | **0.43 W** | `OK` |

The reason is structural rather than a tuning artifact: arming is a one-way door.
`__pm_runtime_disable()` takes away the machinery that would have suspended the device, and a port
cannot suspend while a child below it is awake (`-EBUSY`, `child_count > 0`) — so an awake subtree
that gets armed stays awake for the whole of S5. The wait can only *ask* (`pm_request_idle()`,
refused with `-EAGAIN` while a reference is held and `-EBUSY` while a child is active; an accepted
request then waits out the autosuspend delay), so what it buys is the case that *can* settle but has
not been asked yet — which on this machine is the normal one. It returns the moment the subtree is
settled, and the photographed 2026-10-06 poweroff is what that costs in practice: it spent **100 ms**
of the budget, because the dGPU was already `D3cold` when the module armed. So a generous budget costs
nothing when the GPU is already asleep and at most itself when it cannot settle at all. Phase 1 asks
every listed device to go idle once; only phase 2 spends the budget polling, so the result no longer
depends on the order of `devs=`.

**What decides the rail is measured, not inferred.** The `FINAL` lines come from a
`SYS_OFF_MODE_POWER_OFF_PREPARE` handler (`s5_shield_observe_final()`) that prints after the device
walk and before the firmware poweroff — the last observable moment (`kernel_power_off()`: notifier +
`device_shutdown()` → power-off-prepare → `syscore_shutdown()` → `machine_power_off()`). Read them
as: dGPU `D3cold` with the bridge in `D3hot`/`D3cold` ⇒ the rail was released before the firmware
took over; anything still in `D0` ⇒ the subtree was armed awake, which is what the wait exists to
prevent and, when the wait cannot settle it either (a held reference: `use=`), the case a
shutdown-time fallback has to cover.

The expensive case that remains is a GPU that is **busy** at poweroff — a CUDA job, an external
display, PRIME offload — because a held reference is exactly what no wait can clear, and the shield
would then freeze that GPU awake for ~19 W the whole night. Upstream covers it with a 90 s wait plus
a GRUB `halt` fallback; this machine, with systemd-boot, has no such fallback yet.

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
dmesg | grep s5-shield                    # "ready: 3 target(s), …, wait_ms=20000, FINAL observer=on"
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
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-evidence         # the same numbers as an EVIDENCE.md row, or the reason it refuses
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
Every line below the `FINAL` marker is the module's own output. The arming block is the one measured
on 2026-10-04 (revision 1.4, `wait_ms=0`, which is why it says it is not waiting); revision 1.5 with
the default wait prints the same block plus its own lines — `asking runtime PM to let go and waiting
up to 20000 ms`, any state change it sees while polling, and then either `settled after N ms` or
`not settled after 20000 ms … arming anyway - expect the rail to stay on`. The `FINAL` lines after
them are the shape this revision is expected to print: the measured night (0.43 W) says the subtree
did end up asleep, but those console lines have not been photographed yet — the 1.5 *arming* block
has been, and it is below.

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

The shape to expect on a healthy poweroff: **the wait settles, and the arming block that follows
reports a subtree that is already `D3cold`/`D3hot`, with `FINAL` agreeing.** The `can still be warm`
warnings below are the module being accurate rather than reassuring: they are what 1.4 printed when
it armed with the dGPU awake and no wait — the configuration measured at 18.51 W. With the 1.5
default those two lines should not appear, because the wait either settles the subtree or says out
loud that it could not.

**Photographed once, on 2026-10-06** (the overnight poweroff, 1.5 with `wait_ms=20000`), the arming
block does read like that:

```
s5-shield: poweroff path, 3 device(s) listed
s5-shield: 0000:01:00.0 is in D3cold, not asleep; asking runtime PM to let go and waiting up to 20000 ms for its bridge out of D0
s5-shield: settled after 100 ms: 0000:01:00.0 D3cold, bridge out of D0
s5-shield: state=D3cold … driver=nvidia rpm=suspended use=0 child=0 dis=0 auto=yes parent=suspended
s5-shield: .shutdown of 'nvidia' nulled
s5-shield: state=D3cold … driver=snd_hda_intel rpm=suspended use=0 child=0 dis=0 auto=yes parent=suspended
s5-shield: state=D3cold … driver=pcieport rpm=suspended use=0 child=0 dis=0 auto=yes parent=suspended
s5-shield: bridge: .shutdown of 'pcieport' left in place on purpose
s5-shield: done: runtime PM off on 3, .shutdown nulled on 2, skipped 0
```

Note what did *not* have to happen: the dGPU was **already `D3cold`** when the module armed, so the
20 s budget was spent in 100 ms and only on waiting for the bridge. The wait is not a toll paid on
every poweroff — it is insurance for the poweroff where the subtree is still awake, which is exactly
the one measured at 18.51 W. The original photo is
[`docs/img/console-2026-10-06-overnight.jpg`](docs/img/console-2026-10-06-overnight.jpg).

The `FINAL` lines after `done:` are **still not seen, and the hold is now ruled out as the reason.**
`done:` (the end of the reboot notifier) is the last line on screen; after it the display stays dark
*with the backlight still on* for about five seconds, and then the machine switches off. That is what
it does with `final_hold_ms=8000` (5–8 s) and with the default `final_hold_ms=0` (~5 s), so the
interval is the machine's own S5 transition and **not** the hold — the hold leaves no trace at all,
which means there is no evidence that `s5_shield_observe_final()` ever ran. Two readings stay open:
the display is already down when the observer is called, or the handler is not reached on this
poweroff path. Neither is worth another window. Read the arming block (now measured) and the window;
the `FINAL` line stays a diagnostic this unit does not have.

## Reading the console

Two things tell you everything after a poweroff, and both exist **only on the console**: they are
printed after `journald` has already stopped, so the journal of the previous boot does not have them
(`journalctl -b -1 -k` on this machine shows the boot's `ready:` line and nothing else from the
poweroff). The arming lines (revision 1.4) say *why* each device is where it is; the
`FINAL` lines say where the tree actually ended up, after `device_shutdown()` and before the
firmware poweroff — the moment that decides the rail.

| what you see | what it means | what to do |
|---|---|---|
| the wait reports `settled after N ms`, the arming block shows `D3cold`/`D3hot`, `FINAL` agrees | the subtree was asleep before it was armed, so nothing could wake it: the rail was cut before the firmware took over. This is the **healthy** case with the 1.5 default (0.43 W measured over 11.12 h; the photographed run settled in **100 ms** with the dGPU already `D3cold`). | nothing — this is the fix working |
| the wait reports `not settled after 20000 ms`, and `FINAL` still shows the GPU or the bridge in `D0` | the subtree was armed awake (`use=` a held reference, `child=` an active device below): the rail is very likely still on — 18.51 W was measured exactly this way. | find the holder in the two counters; a held reference is what no wait can clear, so this is the case for a shutdown-time fallback (upstream's 90 s + GRUB `halt`; this machine has none yet) |
| no `FINAL` line at all | not observed here, and the hold is ruled out as the explanation: the dark interval after `done:` is ~5 s both with `final_hold_ms=8000` (5–8 s) and with the default `0`, so it is the machine's own S5 transition rather than the hold — and a hold that leaves no trace is a hold that did not run. Either the display is already down when the observer is called, or the handler is not reached on this path. The other cause: the observer did not register (check `ready: … FINAL observer=on`). | nothing: the shield works without it, and the arming block — which *is* visible — already says whether the subtree was asleep before it was armed |

The fields: `rpm=` is the device's runtime PM status, `use=` is how many references hold it awake
(`-EAGAIN` when the wait asks it to suspend), `child=` is how many devices below it are still active
(`-EBUSY`; a root port with `child=1` is the normal state while its GPU is awake), `dis=` is the
disable depth this module itself raises when it arms, `auto=` is the `power/control` setting, and
`acpi=` is the ACPI power state of the same device — the platform-side half of "is the rail off".
A `D0` at the witness snapshot is **not** an alarm by itself: that snapshot is taken before the
poweroff notifier, and the module's wait runs after it. A `D0` that survives both the wait and the
device walk is the alarm — that is what the `FINAL` line is for.

**Do not set the diagnostic hold.** Two windows measured what it does: `final_hold_ms=8000` gave 5–8 s
of dark screen with the backlight on before the machine switched off, and the default `0` gave about
the same ~5 s. So the hold is not what produced that interval, there is no evidence it ever executed,
and all it adds to a poweroff is a delay nobody can see. Leave it at 0.

## Result

Four judged windows, all on battery with the charger unplugged at both ends, all with the witness
record carrying its raw registers (`docs/EVIDENCE.md` has the table, the gates and the ledger):

| when | configuration | off window | result |
|---|---|---|---|
| 10-04 12:06 → 14:37 | **no shield at all** (`modprobe -r`) | 2.52 h | 51.242 Wh ⇒ **20.33 W** — this unit's own "before" |
| 10-06 10:49 → 13:15 | **1.5, `wait_ms=20000`** (the shipped default) | 2.43 h | 1.031 Wh ⇒ **0.42 W** — the "after", in the baseline's own window: **a 48× drop** |
| 10-04 23:03 → 23:24 | 1.4, `wait_ms=0` | 0.34 h | 6.288 Wh ⇒ **18.51 W** — it armed a subtree that was awake; ledger `FAIL`, check: *the rail was NOT cut* |
| 10-04 23:37 → 10-05 10:44 | 1.5, `wait_ms=20000` | 11.12 h | 4.771 Wh ⇒ **0.43 W** — ledger `OK` |

The first two rows are the pair: same machine, same window length (2.43 h against 2.52 h, and the
shorter one is the shielded one — a shorter window reads *higher* for the same S5, so the residual
difference runs against the fix, not for it), shield off against shield on — **20.33 W to 0.42 W**.
The third row is what earns the rest: it is the **same module binary** as the fourth
(`srcversion 5ABD41F6E01E06E371E5D2F`) with one parameter changed, and it is what turned "the wait
does not pay for itself" into a measured falsehood. The 11.12 h row also sits next to upstream's
closest night on the reference machine, `nocturna-real-v2` (0.46 W over 8.60 h), though as a
different window it may not be subtracted from the baseline under rule 1.

* Chassis cold after the 11-hour window — the symptom this started from, gone.
* The 2026-10-03 50-minute run (~1 W, revision 1.3, `wait_ms=5000`) was the first hint that the wait
  was doing the work; its witness record has since been trimmed by the log cap, which is why
  `rows.tsv` exists and why the rows above are quoted from the ledger rather than from prose.

## Limits

* The wait is a budget, not a guarantee. A subtree that cannot settle — a held reference (`use=`) —
  will not settle however long the budget is, and the module then arms anyway and says so in the
  `not settled … arming anyway` line. That poweroff is no worse than it would have been without the
  shield; it is the case a shutdown-time fallback exists for (upstream: 90 s plus a GRUB `halt`;
  this machine, with systemd-boot, has none yet).
* The wait costs shutdown time exactly when it cannot help: up to `wait_ms` (20 s by default) before
  the same arming happens. It returns the moment the subtree is settled, so a poweroff whose dGPU is
  already asleep pays nothing at all.
* A GPU that is **in use** at poweroff — a CUDA job, an external display, PRIME offload — holds a
  reference, so no wait clears it: the shield freezes it awake and that S5 costs the ~19 W class.
* If a subtree refuses to settle and there is no fallback, the remaining lever is
  `pcie_port_pm=force` on the kernel command line, which is out of scope here.
* The shield is not undone by `modprobe -r`: once armed, the next poweroff is committed. The worst
  case of this class of change is a hang at poweroff — hold the power button ~10 s; the
  filesystems are already unmounted at that point. It cannot hang a reboot: it only arms for
  `SYS_POWER_OFF`.
* If a future kernel changes `struct pci_driver` or the notifier ordering, DKMS still builds it;
  the worst case is a load failure, i.e. the old behaviour.

### History

Revisions 1.0 and 1.1 both shipped a module that silently refused to load its own device list (a
wrong audio class macro, then a `class >> 16` shift against 16-bit macros) — so the fix was inert
and nothing said so. 1.2 fixed the decoding; 1.3 added the wait (step 0); 1.4 turned that wait off
and replaced it with measurement — the arming-time blocking reason and a `FINAL` line at the moment
of no return — and that measurement then proved 1.4's own default wrong, which 1.5 corrects: the wait
is back on by default, because it is the step that makes the rest of the fix reachable. All three
bugs were caught by review, not by testing, which is why
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
| `bin/s5-evidence` | turns one poweroff into one `docs/EVIDENCE.md` row, or refuses with the reason; `--selftest` runs its gates on synthetic records |
| `docs/EVIDENCE.md` | every measured window with its protocol, its gates and what is *not* measured |
| `bin/s5-shield-status` | module loaded? installed source this revision? self-check result, device states |
| `bin/s5-battery` | one-line battery/AC snapshot for a before/after measurement |
| `bin/s5-shield-dryrun`, `bin/s5-logictest` | the two self-checks `install.sh` refuses to skip (the second also compiles and runs the blocker logic) |

Everything here is **GPL-2.0-only** (`LICENSE`); the module declares it with an SPDX tag.
Nothing outside `/usr/src/s5-shield-1.5`, `/etc/modprobe.d`, `/etc/modules-load.d`,
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
