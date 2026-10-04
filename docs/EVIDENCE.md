# Evidence base — `s5_shield` on the HP OMEN 16-ap0xxx (`8E35`)

Instrumented measurements, one row per poweroff, kept to the same discipline as the evidence base
of the project this one borrows its method from
([AnxoCalvo/s5-poweroff-fix, `docs/EVIDENCE.md`](https://github.com/AnxoCalvo/s5-poweroff-fix/blob/main/docs/EVIDENCE.md)):
a row is a **real shutdown** with a battery reading at the last moment before the poweroff and in the
first seconds after the next boot, and it is never quoted without its window length.

Machine for every row below: HP OMEN 16-ap0xxx (`8E35`), BIOS F.13, Ryzen 9 8945HX + RTX 5060 Max-Q,
Arch Linux, kernel 7.2.8-arch1-2, `systemd-boot`, Secure Boot on. The module revision is part of the
row: the fix changed between 1.3 and 1.4 (the wait retired — see README *The fix*).

## How to read these numbers

1. **Raw watts are not comparable across windows of different length** (upstream's rule 1). Every
   window contains a fixed cost E₀ — the power-on, the boot, and the first minutes of a machine that
   draws 24–78 W awake — so a short window always reads higher than a long one for the same S5.
   Quote **Wh and the window**; derive W from those two only against windows of comparable length.
   This machine's E₀ is **not yet pinned**, which is why the rows below are short windows and are
   labelled as such.
2. **A window with the charger connected is void, not "clean".** `bin/s5-evidence` refuses to emit a
   row for one, at either end.
3. **Do not publish a row the tool refused.** Every failure mode here — charger on, a battery that
   went *up*, a boot sample taken minutes after boot, a missing boot record, a 12-minute window —
   still produces a number, and a number with a decimal point looks like evidence.
4. **"What changed" is recorded before the run, not recalled after it.**
   `bin/s5-evidence label "<short>" "<what changed>"` writes `/var/lib/s5-shield/label`; both witness
   records quote it (`label:` / `change:`) and the row is generated from it.
5. **n is stated per configuration.** One row is one shutdown. A configuration with a single row says
   so in its verdict.

## Protocol — how a row is produced

```bash
# before the window
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-evidence label "clean-night-1.4" \
        "revision 1.4, wait_ms=0, shield armed"
/mnt/Shared/Development/Project/Others/s5-shield/bin/s5-battery   # note it, UNPLUG the charger
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
| — | `baseline-no-shield` | shield removed (`sudo modprobe -r s5_shield`), same window as `clean-night-1.4` | *to be measured* | *pending* |
| — | `clean-night-1.4` | revision 1.4, `wait_ms=0` (the wait retired), same window as the baseline | *to be measured* | *pending* |
| — | `wait-5000-1.4` | revision 1.4 with `wait_ms=5000` restored, screening window only | *to be measured* | *pending* — isolates the wait: same code, one parameter |

## What is *not* measured here

* **This unit's pre-fix drain.** It was never measured: the ≈20 W is upstream's measurement on the
  same model, and the ≈16.7 Wh figure in the README is arithmetic from it (20 W × 0.84 h). The
  `baseline-no-shield` row exists to close exactly this gap, and until it exists no row here may be
  compared against a "before" that was taken on another machine.
* **A busy dGPU at poweroff** — CUDA job, external display, PRIME offload. Upstream covers that case
  with a 90 s wait plus a GRUB `halt`; this machine has no fallback yet, and the shield would freeze
  such a GPU awake. No row here covers it, and none should be read as if it did.
* **A whole night with the shield**, at the standard of upstream's `nocturna-real` (0.46 W over
  9.5 h). That is what `clean-night-1.4` is for; the 50-minute row above is not comparable to it
  (rule 1).

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
3. **A diagnostic that only prints is worth more than one that guesses.** Revision 1.3 waited 5 s per
   poweroff for a state change it could not cause; revision 1.4 prints the reason the device is awake
   (`use=`, `child=`, `dis=`) at arming time and the state it ended in (`FINAL`, after
   `device_shutdown()`) — which is what the pending rows are read through.
