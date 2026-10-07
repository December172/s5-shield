# Powering off from the firmware on a machine with `systemd-boot`

This is the piece a `systemd-boot` machine is missing when the dGPU will not settle before a
poweroff. It is the equivalent of GRUB's `halt`: **a boot in which no Linux kernel runs at all**, so
the firmware does its own S5 with the hardware as the boot loader left it, and there is no rail to
cut.

Why that is the thing that matters, measured on the reference machine and written down in
`docs/EVIDENCE.md` of the upstream project: a module that calls `ResetSystem` through EFI from a
*live* kernel leaves S5 at 19.26 W, and writing `PM1a_CNT` by hand without `_PTS(5)` or
`device_shutdown()` leaves it at 26.56 W. Powering off "through the firmware" is not what saves the
measurement; not having booted a kernel is.

## Two ways to do it here

| route | what powers the machine off | what it costs | what it gives you |
|---|---|---|---|
| **built-in** (`bin/s5-halt`, default) | systemd-boot's own "Power Off The System" entry | `auto-poweroff yes` in `loader.conf` (loader v255+). Nothing to build, sign or write to the ESP | the measured path; no marker afterwards |
| **application** (`bin/s5-halt-efi`, optional) | our `efi/s5-halt.c`, three instructions: delete the one-shot variable, `ResetSystem(EfiResetShutdown)` | a build, and a signature if Secure Boot is on | it writes `S5HaltLastRun` before switching off, so "the firmware really did it" can be read afterwards |

Measurements (this unit, 2026-10-07, dGPU pinned awake on purpose, charger out, 0.76 h windows):
`1.007 Wh ⇒ 1.32 W` on the built-in route, against the reference machine's `grub-halt` at 1.05 W
over 45 min. The rows and their verdicts are in [`EVIDENCE.md`](EVIDENCE.md).

## The one thing both routes share: arming is an EFI variable

`LoaderEntryOneShot` is what makes the next boot take that entry, and writing it is the whole
mechanism. Nothing is mounted, no file on the ESP is touched, and no boot entry is added or changed
by arming.

That is also why arming survives the stage where `/boot` is already gone. The upstream project
measured it: `boot.mount` is unmounted 100 ms before `shutdown.target` is reached, which is why its
GRUB branch has to mount `/boot` itself during a shutdown. Here it does not matter. Checked on
2026-10-07, as a normal user so it could not write:

```
$ bootctl --esp-path=/tmp/empty-esp set-oneshot <some-id>
Failed to update EFI variable 'LoaderEntryOneShot-4a67b082-...': Permission denied
```

It reaches the variable write and fails only on permissions. The same check caught a real bug in the
first version of this work: the discovery step refused to arm whenever the ESP did not look mounted,
which is exactly the state it would find in a shutdown — an audit of the code would not have shown
it, and a test that does not cover that stage will not either.

## Anti-loop, with two barriers

The boot loader consumes `LoaderEntryOneShot` when it uses the entry — it is "for the next boot", and
systemd-boot deletes it. As a second barrier the application deletes it too before calling
`ResetSystem`; measured on 2026-10-06, with the poweroff already done, that delete returns an error
because systemd-boot took it first, and there is no loop. The entry's *file* stays on the ESP on
purpose: once the variable is gone it is just another menu entry, and `uninstall.sh` sweeps it.

## Fail safe, always

Whatever is missing is named and the command is not carried out: no `bootctl`, no efivarfs, no
one-shot support in the loader, no signature with Secure Boot on. In every one of those cases the
poweroff continues along its normal path and only the saving is lost. `bin/s5-halt-divert` follows the
same rule: if arming fails, it does **not** divert — a normal poweroff is better than a reboot that
boots.

## Secure Boot

With Secure Boot enabled the firmware only loads images signed by a key it knows, so the application
route needs a signature. It is signed **by you, with your keys** (`sbctl sign`, or `sbsign` with your
own key and certificate); this repository installs no keys and touches no Secure Boot database.
Unsigned, systemd-boot does not load the application, the menu comes back and the usual thing boots:
nothing breaks, but nothing is saved either, which is why `bin/s5-halt-efi install` checks the
signature and refuses to go on if it cannot produce one. The built-in route needs none of this.

## What is tested, and what is not

* **Tested**: the built-in route end to end, twice, with the watt written down (see `EVIDENCE.md`).
* **Tested**: arming from the real shutdown stage, with the ESP unmounted, from a systemd-shutdown
  hook — the menu came up with the built-in entry preselected, which nothing but an armed one-shot
  produces, and the machine switched off and waited for the power button.
* **Tested**: the application route's tooling — build (clang + lld-link), `sbctl` signing, install to
  the ESP, arm with read-back, disarm. The application itself powered a machine off on 2026-10-06
  (photograph: [`img/rehearsal-2026-10-06.jpg`](img/rehearsal-2026-10-06.jpg)) and can be read back
  afterwards through `S5HaltLastRun`.
* **Not tested**: `bin/s5-halt-divert` in a real shutdown. Its branches are exercised in isolation
  (dry run, brake, already-settled, divert), but the decision has never run for real. The README says
  so where you would read about it, and it is installed only on purpose.
* **Not tested on other machines**: the ESP layout here is `/boot`; the tools ask `bootctl` for it, and
  nothing else has been seen. Firmware and boot loaders from other vendors accepting the one-shot
  variable is also untested here.
