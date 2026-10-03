# s5-shield — keep the discrete GPU in D3cold across poweroff

Machine: **HP OMEN Gaming Laptop 16-ap0xxx** (`8E35`), BIOS **F.13**, AMD Ryzen 9 8945HX
(iGPU `amdgpu`) + NVIDIA GB206M / RTX 5060 Max-Q (`nvidia`, open 615.71.09), Arch Linux,
kernel **7.2.8-arch1-2**, ACPI S-states `S0 S4 S5`.

Status: **installed and measured working** on this machine — see *Result*. The repository packages
the module as DKMS `s5-shield/1.3`; the machine's registration moves to it the next time
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

One out-of-tree module, `s5_shield` (381 lines of code in a 642-line file that is mostly the
reasoning, GPL-2.0, `src/s5_shield.c`). It registers a **reboot notifier**, which
`kernel_power_off()` runs *before* `device_shutdown()` (`kernel/reboot.c:303-310`), so by the time
the PCI core walks the device list it has already:

0. **waited for the dGPU to be genuinely asleep** — nudging runtime PM with `pm_request_idle()`
   and polling until it reports `D3cold`, the state in which its rail is off, up to `wait_ms`
   (default 5000);
1. **disabled runtime PM** on the listed devices (`__pm_runtime_disable()`), so the
   `pm_runtime_resume()` above bounces with `-EACCES` instead of powering the GPU up
   (`drivers/base/power/runtime.c:798-808`);
2. **nulled `drv->shutdown`** for the drivers named in `noshut` (`nvidia`, `snd_hda_intel`), so no
   driver teardown runs MMIO against an unpowered device.

Step 0 is the easy one to miss: without it, step 1 can *pin an awake GPU awake*, and a PCI port
cannot sleep while a child below it is awake — which is what has to happen for the slot power
resource (`PG00` / `LNXPOWER:04`) to release the rail. Steps 1 and 2 must go together: 1 alone
leaves `nv_pci_shutdown()` executing against a device in `D3cold`, and that bus-locks this machine.

Safety rails: nothing happens on reboot/halt or while the system runs; `devs=` is mandatory, and
every address is resolved and class-checked (display / audio / PCI bridge only) both at load time
and again at poweroff; `amdgpu`, `nvme`, `ahci`, `xhci_hcd`, networking and the rest are refused
outright; `pcieport` is never added to `noshut` (it governs every port in the machine); a listed
bridge must really be an ancestor of a listed device; if anything is off, the module refuses to
load and you simply get today's behaviour.

## Install / uninstall (root)

```bash
sudo ~/Tools/s5-shield/install.sh     # checks, DKMS build, configs, live reload, /boot fingerprint
sudo ~/Tools/s5-shield/uninstall.sh   # back to the previous behaviour, nothing left behind
```

`install.sh` runs the two self-checks in `bin/` first and **refuses to install** if the module
would not accept this machine's devices. DKMS then rebuilds it on every kernel update.
`/etc/modprobe.d/s5-shield.conf` holds the device list, the driver list and `wait_ms`.

## Verify and measure

```bash
~/Tools/s5-shield/bin/s5-shield-status    # loaded? installed source == this revision? device states
~/Tools/s5-shield/bin/s5-shield-dryrun    # what the module would do, read out of the C source
dmesg | grep s5-shield                    # "ready: 3 target(s), noshut=…, wait_ms=5000"
```

A **reboot proves nothing** (the shield acts on poweroff only), and uptime drowns the effect: this
laptop draws 24–78 W awake, i.e. 4–8% of the battery per five minutes. Measure the off window and
nothing else:

```bash
# 1. charge it, then UNPLUG the charger (AC masks everything)
~/Tools/s5-shield/bin/s5-battery
# 2. sudo systemctl poweroff        (a real poweroff, not a reboot)
# 3. wait 30-60 min, power on, then:
~/Tools/s5-shield/bin/s5-verdict          # watts across the OFF window
cat /var/log/s5-shield-witness.log        # what state the dGPU was in, and when
```

The optional **shutdown witness** (`sudo ~/Tools/s5-shield/witness.sh install`, refreshed
automatically by `install.sh`) records the dGPU, its root port and the port's ACPI state with
timestamps on the way into S5, plus the battery at both ends of the off window. It only reads
sysfs — no `lspci`, no `nvidia-smi`, nothing that could wake the device being measured.

The kernel prints its own account as the machine goes down, visible even with `quiet loglevel=3`:

```
s5-shield: 0000:01:00.0 is in D0, not asleep; asking runtime PM to let go and waiting up to 5000 ms for D3cold
s5-shield: 0000:01:00.0 reached D3cold after 300 ms
s5-shield: 0000:01:00.0 state=D3cold driver=nvidia
s5-shield: done: runtime PM off on 3, .shutdown nulled on 2, skipped 0
```

## Result (2026-10-03)

First instrumented poweroff: 11:51:04 → 12:41:07, **50 minutes off**, on battery with the charger
unplugged the whole time.

* The witness ran (that is itself a fix — see *History*) and recorded the dGPU **still in `D0`
  after 6 s with no holders**: the nvidia driver does not runtime-suspend it on its own once its
  clients are gone. Step 0's `pm_request_idle()` is what asks for that suspend, a moment *after*
  the witness stops watching.
* Battery at the last moment before poweroff **64.743 Wh (82.8%)**; first sample after boot
  **80%** — 2.2 Wh, of which the three minutes of uptime in between are worth 1.2–2.9 Wh. So the
  50 minutes the machine was actually off cost **at most ~1 Wh (≈1 W)**, against the ≈16.7 Wh the
  old behaviour produced over the same window.
* Chassis cold — the symptom this started from, gone.

## Limits

* If the dGPU does not reach `D3cold` within `wait_ms`, the module says so and that poweroff is no
  worse than before: the rail was never released, so there was nothing to preserve. The remaining
  lever would be `pcie_port_pm=force` on the kernel command line, which is out of scope here.
* The wait costs nothing when the GPU is already asleep and at most `wait_ms` when it is not;
  `wait_ms=0` in `/etc/modprobe.d/s5-shield.conf` removes it.
* The shield is not undone by `modprobe -r`: once armed, the next poweroff is committed. The worst
  case of this class of change is a hang at poweroff — hold the power button ~10 s; the
  filesystems are already unmounted at that point. It cannot hang a reboot: it only arms for
  `SYS_POWER_OFF`.
* If a future kernel changes `struct pci_driver` or the notifier ordering, DKMS still builds it;
  the worst case is a load failure, i.e. the old behaviour.

### History

Revisions 1.0 and 1.1 both shipped a module that silently refused to load its own device list (a
wrong audio class macro, then a `class >> 16` shift against 16-bit macros) — so the fix was inert
and nothing said so. 1.2 fixed the decoding; 1.3 added step 0 above. Both bugs were caught by
review, not by testing, which is why `bin/s5-shield-dryrun` and `bin/s5-logictest` now read the
accept-list and the shift **out of the C source** and check them against the live hardware instead
of mirroring them. The witness unit had a third such bug: with `DefaultDependencies=no` and only
`Before=shutdown.target`, systemd never stopped it, so it never recorded anything.

## Files

| path | what it is |
|---|---|
| `src/s5_shield.c` | the whole fix: one reboot notifier, 381 lines of code, GPL-2.0 |
| `src/{Makefile,dkms.conf}` | build and DKMS packaging for it |
| `etc/modprobe.d/s5-shield.conf` | the device list (`devs=`), the driver list (`noshut=`), `wait_ms=` |
| `etc/modules-load.d/s5-shield.conf` | loads the module at boot |
| `install.sh`, `uninstall.sh` | the only two things you have to run |
| `witness.sh` | installs/removes the optional shutdown witness |
| `systemd/s5-shutdown-witness.service` | the witness unit (boot record + shutdown record) |
| `bin/s5-shutdown-witness` | the witness itself: reads sysfs only, never touches a driver |
| `bin/s5-verdict` | pairs the shutdown and boot records, prints the watts across the off window |
| `bin/s5-shield-status` | module loaded? installed source this revision? where is each device |
| `bin/s5-battery` | one-line battery/AC snapshot for a before/after measurement |
| `bin/s5-shield-dryrun`, `bin/s5-logictest` | the two self-checks `install.sh` refuses to skip |

Everything here is **GPL-2.0-only** (`LICENSE`); the module declares it with an SPDX tag.
Nothing outside `/usr/src/s5-shield-1.3`, `/etc/modprobe.d`, `/etc/modules-load.d`,
`/lib/modules/<kver>/updates/dkms` and the optional witness unit is touched.

## Credits

* Diagnosis and method for this exact model: **Anxo Calvo**,
  [s5-poweroff-fix](https://github.com/AnxoCalvo/s5-poweroff-fix); upstream analysis and the
  never-merged fix: **Mario Limonciello** and the LKML thread linked above.
* The module, the tooling and this document were written by the **deepseek-flash** coding agent
  (DeepSeek Harness) in session with december172, 2026-10-03 — including finding two bugs in its
  own earlier revisions, the third in the witness unit, and the instrumented test that settled
  whether the fix works.
