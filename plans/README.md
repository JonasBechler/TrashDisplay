# Plans: clearing the hero on "Ja" with a celebration

Three independent implementation plans, each written by a separate Opus 5.5
agent from the same brief but with a different assigned direction, so they do
not converge on one idea:

| Plan | File | Direction |
|------|------|-----------|
| A | [A-particle-celebration.md](A-particle-celebration.md) | Burst of hearts / stars / confetti pieces over the screen, then the next view |
| B | [B-done-overlay.md](B-done-overlay.md) | Big "Erledigt" stamp with checkmark over the hero, entrance and exit, then the next view |
| C | [C-hero-exit-transition.md](C-hero-exit-transition.md) | The hero bars themselves animate away; lightest option, no new assets if possible |

## Decision (2026-10-16)

All three were implemented on separate branches and tried on the device the
same evening. **Plan B, the done overlay, was chosen** and merged to `main`,
with hardening on top: the acknowledgement loader no longer stalls one second
at boot (`File::parseInt` timing out at end of file), a corrupt
acknowledgement beyond the hero window is cleared instead of hiding the
calendar, and a fetch window waits while the card is up. Plans A and C stay
on their branches for reference (`plan-a-particles`, `plan-c-hero-exit`,
both based on commit 0ab0444); their documents below are unchanged.

The three plans were written and implemented without a compiler: the sandbox
that produced them has no PlatformIO, so every LVGL call was checked against
the v9.2.2 sources and the code was syntax-checked with a host g++ against
the LVGL headers. The device builds were done by the owner.

## Comparison

| | A: particles | B: done overlay | C: hero exit |
|---|---|---|---|
| What the user sees | Hero vanishes, green "Erledigt!" with bin names, 24 confetti / heart / star pieces burst from where the bars were and fall off screen | Green card over the dimmed hero: white circle pops, check mark, "Erledigt", bins, amber "Danach: ..." hint if another hero follows; fades to the next view | Header turns green "Erledigt!", a line strikes each bin name, bars slide off to the right one by one; the next view fades or slides in from the left |
| Time until next view | about 1.6 s | about 2.1 s (tap skips after 0.4 s) | 1.1 s with one bar, 1.6 s with four |
| New assets | 2-glyph FontAwesome font, about 0.5 KB | 1-glyph 48 px check font, about 1 KB | none |
| LVGL heap | about 1 KB during the burst, less than the hero itself | 8 permanent objects, about 2 KB | about 2 KB peak, freed afterwards |
| Code size (estimate) | 2 to 3 KB | about 3 KB | about 1.5 KB |
| CPU | boosts to 240 MHz for the burst | 80 MHz | 80 MHz, boost only as fallback |
| Drawing method | custom draw callback on one overlay object | plain objects and labels, no layers | animates the existing hero objects |
| Acknowledgement fix | set of up to 4 days, one per line in `/ack.txt` | high-water mark: one day covers all earlier pending days, same file | high-water mark, plus a guard against a corrupt file |
| Undo | lifts the one named day | steps back one day | lifts all days (3-slot variant offered) |
| Optional LED / sound | neither | LED left to the owner, no sound | LED behind a build flag, sound advised against |

All three independently found the same extra edge case: a "Ja" still pressed
when the 15 s dialog timeout closes the dialog is delivered on release with
`dialog_day == -1`, which writes "-1" into `/ack.txt` and silently drops the
acknowledgement. Each plan adds a guard. All three also reject acknowledging
the whole hero window at once, because that would hide Tuesday's bin on
Monday evening.

## Device state on 2026-10-16

The owner's build fails with every `Config.h` constant undeclared, so the
local `src/Config.h` predates this firmware. The device therefore runs an
older build, and the "hero persists" observation comes from that build. Fix:
copy `src/Config_example.h` to `src/Config.h` and restore the WiFi and
address values, then flash with `pio run -e esp32-2432S028R -t upload`.

## Is the done button a mock?

No. The flow on "Ja" is real and complete in `src/main.cpp`:

1. `on_dialog_yes` (line 481) calls `ack_set(dialog_day)` (line 491), which
   stores the day in `acked_day` and writes it to `/ack.txt` in LittleFS
   (`src/trash_functions.h`, `ack_set`). The mark survives a reboot.
2. `dialog_close()` (line 493) hides the dialog on the top layer.
3. `update_display()` (line 494) calls `lv_obj_clean(content)` (line 288),
   which deletes every hero object, then rebuilds the view. The loop that
   picks the next date per type skips `d == acked_day` (line 313), so the
   acknowledged day is treated as collected and its hero cannot come back.
   The grey "Erledigt: <date>" label appears bottom-right.

So in code the hero is cleared on the next LVGL refresh (33 ms).

## Why the hero can persist on a real device

The owner reports that the hero stays no matter how often "Ja" is pressed.
Once an acknowledgement is set, a tap with no hero on screen opens the undo
dialog ("Markierung aufheben?") instead, so "I can keep confirming" means the
acknowledgement is never set, or the acknowledged day's hero is still being
rendered. Two causes, both real:

1. **Confirmed on hardware (2026-10-16): "Ja" never reached the handler.**
   The firmware's `touch_read_rotated` wrapper applied the 270° rotation that
   LVGL 9.2.2 already applies itself in `indev_pointer_proc` (`lv_indev.c`),
   so every tap was rotated twice. The top-left corner logged 29,205, the
   bottom-left 210,210, and the top-right corner produced "Y is -48" warnings.
   Under that mapping a tap on "Ja" lands on the card text (nothing happens)
   and a tap on "Nein" lands on the backdrop (cancel), so the acknowledgement
   could never be set. The wrapper is removed; after flashing, the corners
   read 36,33 / 287,31 / 290,210 / 32,212 clockwise from top-left, which
   confirms the mapping. The pinned esp32-smartdisplay commit (a49bf44) was also checked:
   it rotates only the LCD panel, and its XPT2046 driver scales X to 240 and
   Y to 320, which is the portrait frame LVGL expects.
2. **Latent, will show once "Ja" works: single-slot acknowledgement with
   consecutive pickup days.** `acked_day` is one `int32_t`
   (`src/trash_functions.h`, `ack_set` overwrites it). Konstanz pickup days
   can be consecutive (Bio Monday, Rest Tuesday). On Sunday the Monday hero
   is acknowledged, line 354 then finds Tuesday within `hero_from_days` and
   renders the Tuesday hero. Acknowledging that one overwrites `acked_day`
   with Tuesday, so Monday is no longer skipped and the Monday hero comes
   back. The two heroes alternate forever. Telltale: the "Erledigt" label is
   present and its date flips between two values. Every plan includes a fix.

A tap on "Ja" must print this on the serial console:

```
day N acknowledged
```

Nothing beyond the corner readings was verified on hardware; PlatformIO is
not installed in the sandbox that produced these plans.

## Implementation branches (2026-10-16)

Each plan is implemented on its own branch, all based on commit 0ab0444
(`touch-fix-test-hooks`: touch fix, serial test hooks, these plans). Each
branch contains its own acknowledgement fix and the late-"Ja" guard. None has
been compiled for the ESP32; each passed a host `g++ -std=gnu++11 -Wall`
syntax check against the LVGL 9.2.2 headers with stand-in Arduino headers.

| Plan | Branch | Commit | Extra test keys (besides h, 0, x, u) |
|------|--------|--------|--------------------------------------|
| A | `plan-a-particles` | 2864dfa | `c` replay the burst, `m` LVGL memory report |
| B | `plan-b-done-overlay` | 98a66bd | `s` single hero without "Danach", `w` fetch window now |
| C | `plan-c-hero-exit` | 06d689e | `1` one bar, `4` four bars |

Plan B is on `main` (branch `plan-b-done-overlay` was merged and deleted).
To try A or C: `git checkout <branch>`, then
`pio run -e esp32-2432S028R-test -t upload && pio device monitor`. Press `x`
between scenarios: on B and C the mark covers every earlier day, on A it is a
set of up to four days.
