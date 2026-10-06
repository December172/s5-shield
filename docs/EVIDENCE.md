# Evidence base — `s5_shield` on the HP OMEN 16-ap0xxx (`8E35`)

Instrumented measurements, one row per poweroff, kept to the same discipline as the evidence base
of the project this one borrows its method from
([AnxoCalvo/s5-poweroff-fix, `docs/EVIDENCE.md`](https://github.com/AnxoCalvo/s5-poweroff-fix/blob/main/docs/EVIDENCE.md)):
a row is a **real shutdown** with a battery reading at the last moment before the poweroff and in the
first seconds after the next boot, and it is never quoted without its window length.

Machine for every row below: HP OMEN 16-ap0xxx (`8E35`), BIOS F.13, Ryzen 9 8945HX + RTX 5060 Max-Q,
Arch Linux, kernel 7.2.8-arch1-2, `systemd-boot`, Secure Boot on. The module revision is part of the
row, and so is `wait_ms`: 1.3 shipped 5000, 1.4 shipped 0, **1.5 ships 20000**, and the difference
between 0 and non-zero is the difference between 18.51 W and 0.43 W on this machine (see
*Diagnostics* and README *The fix*).

## How to read these numbers

1. **Raw watts are not comparable across windows of different length** (upstream's rule 1). Every
   window contains a fixed cost E₀ — the power-on, the boot, and the first minutes of a machine that
   draws 24–78 W awake — so a short window always reads higher than a long one for the same S5.
   Quote **Wh and the window**; derive W from those two only against windows of comparable length.
   This machine's E₀ is **not yet pinned**, which is why every row states its window. The one legal
   pair under rule 1 is still owed: `baseline-no-shield` (2.52 h, no shield) against a clean window
   of the *same* length with 1.5. The 11.12 h row below is a stronger result but a different window,
   so it may not be subtracted from the baseline.
2. **A window with the charger connected is void, not "clean".** `bin/s5-evidence` refuses to emit a
   row for one, at either end.
3. **Do not publish a row the tool refused.** Every failure mode here — charger on, a battery that
   went *up*, a battery that did not move **at all**, a boot sample taken minutes after boot, a
   missing boot record, a 12-minute window — still produces a number, and a number with a decimal
   point looks like evidence. `bin/s5-evidence` now refuses the zero-delta case by name; see lesson 4.
4. **"What changed" is recorded before the run, not recalled after it.**
   `bin/s5-evidence label "<short>" "<what changed>"` writes `/var/lib/s5-shield/label`; both witness
   records quote it (`label:` / `change:`) and the row is generated from it.
5. **n is stated per configuration.** One row is one shutdown. A configuration with a single row says
   so in its verdict.

## Protocol — how a row is produced

```bash
# before the window
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-evidence label "clean-window-1.5" \
        "revision 1.5, wait_ms=20000, shield armed"
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-battery   # note it, UNPLUG the charger
# start from a partly discharged pack, NOT from 100% (lesson 4), and not from below
# ~25%: the window has to survive its own worst case without the EC cutting early.
sudo systemctl poweroff        # watch the console: the FINAL lines are the last thing printed
# after the next boot
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-evidence  # the row, or why it refuses
```

* `bin/s5-verdict` prints the same arithmetic as prose; `bin/s5-evidence` computes it **independently**
  and refuses the row if the two disagree by more than 0.02 W.
* Both witness records quote the raw registers (`battery_raw:`), so any published watt can be
  recomputed from the log instead of taken on trust.
* One line per judgeable window is appended at boot to `/var/lib/s5-shield/rows.tsv`, which is
  **never trimmed** (the witness log is capped, and it did trim the record of the 11:51 run below
  before it could be published). Columns:
  `recorded_at  shutdown_ts  window_h  energy_Wh  watts  verdict  label  kernel  wait_ms  srcversion  ac`.
* `bin/s5-evidence --selftest` runs the gates against synthetic records, without root. It is the
  reason the `s5-verdict` off-window bug was caught before it could bless a night (lesson 2).

## All measurements

| date | label | what changed | result | verdict |
|---|---|---|---|---|
| 10-03 | `clean-50min-1.3` | revision 1.3: shield armed with the dGPU in `D0`, `wait_ms=5000` ran to its full budget and gave up | ≤1 Wh / 0.83 h ⇒ **≈1 W** (raw window 2.2 Wh, of which 1.2–2.9 Wh is the uptime inside it) | **CLEAN** — n=1. Source: README *Result*; the witness record itself was trimmed by the log cap, so its raw registers can no longer be re-read |
| 10-03 | `ac-on-void` (23:56 → 09:38) | revision 1.3, charger connected throughout | refused by the gate (both ends) | **VOID** — quoted only to show the gate works |
| 10-04 | `baseline-no-shield` | shield **removed** (`sudo modprobe -r s5_shield`), charger out, 2.52 h window — this unit's own "before" | **51.242 Wh / 2.52 h ⇒ 20.33 W** | **POISONED** — deliberate: the row measures the drain with nothing shielding it, and it lands in the same 18.7–24 W class as the reference machine. Raw registers: appendix of the fork's `docs/EVIDENCE-second-unit.md`, and the untrimmed `/var/lib/s5-shield/rows.tsv` |
| 10-04/05 | `probe-wait-20s` | revision 1.4 with `wait_ms=20000` restored — the parameter 1.5 now defaults to — charger out at both ends | **4.771 Wh / 11.12 h ⇒ 0.43 W** (the boot itself is inside that window) | **CLEAN** — n=1, ledger verdict `OK`. Raw registers in `/var/lib/s5-shield/rows.tsv` and the witness log. Not comparable to the 2.52 h baseline under rule 1 (a longer window flatters the number, and it still read 0.43 W); it *is* comparable to upstream's `nocturna-real` (0.46 W over 9.5 h) |
| — | `clean-window-1.5` | revision 1.5, `wait_ms=20000` (the default), **the same 2.52 h window** as the baseline | *to be measured* | *pending* — the row that completes the legal pair |

**The wait is not optional, and that was measured the hard way.** Revision 1.4 shipped `wait_ms=0`
on the theory that the dGPU falls asleep later anyway, inside `device_shutdown()`. The diagnostics
that same revision added are what disproved it — one parameter, the same module binary
(`srcversion 5ABD41F6E01E06E371E5D2F`), the same arm-time state (dGPU `D0`, root port `D0`, audio
`D3hot`, no holders):

| `wait_ms` | window | off draw | ledger |
|---|---|---|---|
| `0` | 0.34 h | **18.51 W** | `FAIL` — the boot check: *the rail was NOT cut* |
| `20000` | 11.12 h | **0.43 W** | `OK` |

The mechanism is why this is a step and not a tuning knob: `__pm_runtime_disable()` takes away the
ability to suspend, and a port cannot suspend while a device below it is awake (`-EBUSY`,
`child_count > 0`), so an awake subtree that gets armed stays awake for the whole of S5. Revision 1.5
therefore ships `wait_ms=20000`. Note what this does *not* contradict: upstream closed the equivalent
patch on 2026-10-04 because *their* policy already waits up to 90 s before their module is loaded, so
a wait inside the module adds nothing **there**. Both statements hold at once — on a machine with no
policy layer, the wait is the only thing that can settle the subtree.

## Diagnostics (not rows)

Windows below the 0.5 h gate are not rows and are never quoted as if they were, but two of them
decided something, so they are kept here (the ledger keeps them regardless — that is what it is for):

| when | config | window | result | what it decided |
|---|---|---|---|---|
| 10-04 23:03 → 23:24 | 1.4, `wait_ms=0`, shield loaded and armed | 0.34 h | 6.288 Wh ⇒ **18.51 W**, ledger `FAIL` | arming an awake subtree with no wait leaves the rail on — this is the measurement that put the wait back in 1.5 |
| 10-04 17:31 → 17:32 and 20:24 → 20:24 | 1.4, charger connected at both ends | 0.5 and 0.0 min | not judged; no row emitted | the charger gate and the backward check working as designed — nothing may be read from either window |
| 10-06 00:21 → 10:27 | 1.5, `wait_ms=20000`, charger out, the pack at **100%** when the window opened | 10.10 h | `energy_now` byte-identical at both ends: **0.000 Wh ⇒ 0.00 W**, and the tools published it | the gauge stayed pinned at full and never integrated the S5 draw (`voltage_now` still fell 12.194 → 11.696 V, and the reading moved as soon as the machine was back under load). The rail was cut — a ~20 W S5 would have flattened this pack in ~4 h, and it booted after 10 — but the window contains no number, so it is not a row. This is what made the zero-delta gate necessary |

## What is *not* measured here

* **This unit's pre-fix drain** is no longer borrowed from anywhere: the `baseline-no-shield` row
  above is this unit's own "before" (20.33 W over 2.52 h, shield removed). What is still missing is
  the clean window of the same length — until that row exists, the pair is not complete and no
  before/after claim may be made from it.
* **A busy dGPU at poweroff** — CUDA job, external display, PRIME offload. Upstream covers that case
  with a 90 s wait plus a GRUB `halt`; this machine has no fallback yet, and the shield would freeze
  such a GPU awake. No row here covers it, and none should be read as if it did.
* **A whole night after a normal day's use.** The 11.12 h row above *is* an overnight window and
  reads 0.43 W, next to upstream's `nocturna-real` (0.46 W over 9.5 h) — but it followed an idle
  evening, and one row is one shutdown. A night that follows a day of real work is still unmeasured,
  as is any window longer than the ~11 h this battery can carry unshielded.

## Lessons, written down

1. **A capped log is not an archive.** Keeping the tail is right for a rotating file and wrong for the
   record you intend to publish: the 11:51 run of 2026-10-03 survives only as README prose because
   the cap trimmed its witness block weeks before this file existed. Hence `rows.tsv`, which is
   appended to and never trimmed.
2. **A verdict tool that cannot disagree with itself is not a check.** `s5-verdict` read the
   off-window battery with a `tail -1` over a block that also contained the *boot* record, so it
   compared the boot record against itself: every poweroff came out `0.00 W — CLEAN`. It was found by
   the fixtures in `bin/s5-evidence --selftest` (case `clean night, 0.5 Wh / 8 h`) and fixed the same
   hour — before any row in this file was written. Two independent computations that must agree
   (0.02 W tolerance) is now part of publishing a row.
3. **A diagnostic that only prints is worth more than one that guesses — and it will eventually
   contradict you.** Revision 1.4 replaced the wait with two read-only diagnostics, on the theory that
   the device walk releases the dGPU anyway. Printing the blocking reason at arming time and the state
   at the moment of no return is what let the very next measured poweroff show the theory was wrong
   (18.51 W with no wait against 0.43 W with it), instead of leaving two plausible stories and no way
   to choose. A measurement that cannot embarrass its author is not measuring anything.
4. **A gauge at 100% is not a gauge.** The 2026-10-06 overnight window opened with the pack full and
   closed with `energy_now` reading exactly the same value — 81083000 µWh at both ends of 10.10 h,
   `capacity=100` both times, while `voltage_now` fell 12.194 → 11.696 V and the reading only started
   moving once the machine was back under load. The tools turned that into **0.00 W — CLEAN**: a
   perfect number for a window with no measurement in it, and precisely the shape of failure rule 3
   warns about. Both tools now refuse a zero delta by name (`bin/s5-evidence`, and the witness no
   longer writes a ledger line for one). The lesson is not "the gauge lies": it is that an instrument's
   *range* is part of the protocol, and a window that starts at the top of the range measures nothing.
