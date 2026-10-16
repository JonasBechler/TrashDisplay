# Plan B: done overlay

The user taps "Ja". In the same frame, the grey dialog card is replaced by a dark green card centred on the still-dimmed hero. On the left a white circle grows from 20 to 76 px in 0.3 s with a slight overshoot, and a green check mark fades in inside it. To its right "Erledigt" is set in `montserrat_de_32`. Below that are the bins just marked ("Biomüll"). If another pickup is due within two days, an amber line follows: "Danach: Restmüll übermorgen". The hero header and bottom rows stay visible, dimmed, around the card, so the stamp sits on the thing it confirms.

After 1.8 s the card disappears, or earlier if the user taps anywhere after the first 0.4 s. In that same frame the content behind is rebuilt, and the black veil lifts over 0.32 s to reveal the next view. That is the list, or for consecutive pickup days the next hero, plus the grey "Erledigt: Mo 05.10." label bottom-right. The whole sequence takes about 2.1 s. Lifting an acknowledgement ("Markierung aufheben?") still shows no overlay. This plan also fixes the single-slot acknowledgement that makes consecutive heroes alternate. That bug is the reason the owner sees the hero "persist no matter how often Ja is pressed".

## Current behaviour

- A tap on the screen with a hero shown opens the dialog for `shown_hero_day` (`src/main.cpp:547-557`). The dialog is a full-screen 70 % black backdrop plus a card on `lv_layer_top()` (`src/main.cpp:590-600`, opacity at `:594`).
- `on_dialog_yes` (`src/main.cpp:481-495`) calls `ack_set(dialog_day)` (`src/trash_functions.h:744-755`, sets `acked_day` and writes `/ack.txt`). It then calls `dialog_close()` (`src/main.cpp:464-469`) and `update_display()`.
- `update_display()` calls `lv_obj_clean(content)` (`src/main.cpp:288`). It skips `d == acked_day` (`:313`) and picks hero or list (`:354-362`). So in code the hero *is* cleared on the next refresh, roughly 33 ms later plus one full-screen frame. The change is a hard cut. The only cue that the tap worked is the 12 px grey `ack_label` (`:383-395`, created `:664-668`).
- **Bug, consecutive days (confirmed by the owner):** `acked_day` is one `int32_t` (`src/trash_functions.h:728`) and `ack_set` overwrites it (`:746`). Take Sunday with Bio on Monday and Rest on Tuesday:
  1. Ja on the Monday hero sets `acked_day` = Mon. The Tuesday hero renders because delta 2 ≤ `hero_from_days` (`src/main.cpp:354`).
  2. Ja on the Tuesday hero sets `acked_day` = Tue. Monday is no longer skipped at `:313`, so the Monday hero comes back.
  3. The two heroes alternate forever, and the label shows the wrong day. The undo dialog is only reachable without a hero (`:553-563`), so the user cannot escape the loop.
- **Race, from code reading only:** a dialog nobody answers closes after 15 s (`:471-474`), and `dialog_close` sets `dialog_day = -1` (`:468`). LVGL 9.2.2 sets `LV_OBJ_FLAG_PRESS_LOCK` on every object (`lv_obj.c:498`). It also sends `LV_EVENT_CLICKED` to the pressed object on release without checking `HIDDEN` (`lv_indev.c:1358`). So a "Ja" pressed just before the timeout and released after it runs `ack_set(-1)`. That writes "-1" and silently lifts any active acknowledgement.
- Other callers of `update_display()`:
  - boot (`:703`), and when time first becomes valid (`:744`);
  - every 60 s (`:775-779`);
  - after every fetch window (`:772`). The window blocks `loop()` for seconds and calls `lv_refr_now` (`:753`, `:758`).
- The press on "Ja" already sets `touch_wake_until_ms` to now + 20 s (`:440-446`, `:797-798`), so a 2 s overlay is lit in a dark room.

## Design

**Rendering rules for this panel.** The pinned smartdisplay allocates one 19 200 px partial buffer (`lvgl_panel_ili9341_spi.c`, `lv_display_set_buffers(..., NULL, ...)`) and byte-swaps it in the flush callback. Render and SPI transfer therefore run one after the other, and a full screen is 4 chunks × 38.4 KB. At 24 MHz that is ≥ 51 ms of SPI per full frame. Two consequences:
- Animate only small areas. Accept a few full-screen frames at the start and end.
- Use no `opa`, `transform_scale/rotation` or `clip_corner` on overlay objects:
  - A transformed layer is ARGB8888 and is not chunked (`lv_refr.c:858-885`, `:1031-1068`). Scaling the 296 px card needs ≥ 296×70×4 ≈ 83 KB per draw chunk, more than the whole 64 KB pool. On failure LVGL only logs "Try later" (`lv_draw.c:439-443`).
  - `lv_obj_fade_in/out` set `opa`. That forces a simple layer made of 24 KB ARGB chunks, plus a second blend pass on every chunk.
  - Only `bg_opa`, `text_opa` and size are animated. None of these creates a layer (`lv_obj_style.c:962-969`).

**Visual.** The overlay is built once on `lv_layer_top()` after the dialog, so it sits above it, and is shown on demand like the dialog.
- Backdrop: 320×240, black, `bg_opa` 70 %, the same as the dialog, so swapping dialog for card does not flash the hero. It is clickable and absorbs every tap. This also makes it independent of touch calibration.
- Card: 296 px wide, height `LV_SIZE_CONTENT` (137-179 px), centred, background `0x2E7D32`, radius 10, padding 12, flex column, row gap 6, not clickable. White text on this green is about 5.1:1, against about 3.3:1 on the "Ja" green `0x43A047`.
  - Row (240×84, transparent):
    - White circle, 76 px, `LV_RADIUS_CIRCLE`, at x 8.
    - Check glyph inside the circle, 48 px, colour `0x2E7D32`.
    - "Erledigt" in `montserrat_de_32`, white, at x 102. Its measured advance width is 129 px.
  - Types line: `montserrat_de_20`, `0xE8F5E9`, width 272, centred, wraps. Shows `types_due_on(day)`; "Gelber Sack, Altpapier" is about 225 px.
  - Hint line: `montserrat_de_16`, amber `0xFFE082`, width 272, centred, wraps. Hidden unless the next pending day is within `hero_from_days`.

```
+------------------------------------------+
|   (  ✓  )   Erledigt                     |   circle pops 20 -> 76 px
|              Biomüll                     |
|      Danach: Restmüll übermorgen         |   amber, only for a near hero
+------------------------------------------+
```

**Check glyph.** Generate a one-glyph font, `src/fonts/symbol_check_48.c`, holding `LV_SYMBOL_OK` (U+F00C) from `FontAwesome5-Solid+Brands+Regular.woff`. The woff ships in lvgl v9.2.2 `scripts/built_in_font/`, and LVGL builds its own symbol fonts from it with code point 61452. It costs about 1 KB of flash and no RAM.

Rejected alternatives:
- Two rotated `lv_obj` strokes: each needs its own transformed ARGB layer, they redraw on every fade frame, and software-transform antialiasing on this panel is unknown.
- `LV_FONT_MONTSERRAT_48 1`: about 100 KB of flash (estimated) for one glyph.
- Plain text only: not recognisable from across the room.

**State machine** (all times from the "Ja" click; constants live in `main.cpp`, not `Config.h`, so existing user `Config.h` files still compile):

| State | Entered | Screen | Tap | `update_display()` from `loop()` | Leaves |
|---|---|---|---|---|---|
| `OV_IDLE` | boot, fade done | normal | normal | runs | Ja → `OV_STAMP` |
| `OV_STAMP` | t=0 | card; circle pops at 80-380 ms (`lv_anim_path_overshoot`, peak ≈ 78 px) | ignored before 400 ms (release bounce of "Ja"), then → reveal | deferred | pop done → `OV_HOLD`; timer/tap → `OV_REVEAL` |
| `OV_HOLD` | ~380 ms | static card, 0 px redrawn | → reveal | deferred | 1800 ms timer or tap → `OV_REVEAL` |
| `OV_REVEAL` | 1800 ms or tap | card hidden, content rebuilt in the same frame, `bg_opa` 70 % → 0 over 320 ms (`ease_out`) | absorbed | runs, harmless | fade done → `OV_IDLE` (~2120 ms) |

**When `update_display()` runs.** The content is rebuilt at the start of the reveal, not when "Ja" is pressed. Rebuilding early would put the next hero, for example "Übermorgen: Restmüll", under an "Erledigt" stamp, which reads as "Restmüll done". While the state is `OV_STAMP` or `OV_HOLD`, `update_display()` returns early and logs it, because the reveal re-renders anyway. The deferral stops after 10 s (`ov_stuck_ms`): it then force-closes the overlay, so a bug cannot freeze the calendar.

A fetch window starting mid-overlay draws the card through `lv_refr_now` and blocks the loop. The card stays frozen for the window, and the overdue hold timer reveals right after. Transition functions check the state, so a late timer after a forced close does nothing.

**Consecutive pickup days.** With the acknowledgement fix (step 1), Ja on the Monday hero on Sunday shows "Erledigt / Biomüll / Danach: Restmüll übermorgen". The reveal brightens the "Übermorgen: Restmüll" hero with "Erledigt: Mo 05.10." bottom-right. Ja on that hero shows "Erledigt / Restmüll" with no hint, then the list with "Erledigt: Di 06.10.". Monday never comes back.

## Implementation steps

1. **`src/trash_functions.h`, after `ack_active`: acknowledgement covers its day and all earlier pending days.** This is the fix the coordinator required. It behaves like a set of acknowledged days, each expiring at its own 10:00, using the existing one-int `/ack.txt`. Existing files keep loading unchanged.
   - Rejected: acknowledging every pickup in the hero window at once, because Ja on the Bio hero would also hide the Rest reminder.
   - Also change `src/main.cpp:313` to `if (!pickup_pending(d, today, tmnow.tm_hour)) continue;`.
   - In the undo branch of `on_dialog_yes`, call `ack_step_back(today, hour)` instead of `ack_clear()`.

```cpp
// An acknowledgement covers its day and every earlier one: the hero always shows the
// earliest pending day, so earlier pending days were acknowledged before. One int thus
// stands for a set of days that each drop out at their own rollover (d < today rule).
static bool pickup_pending(int32_t d, int32_t today, int hour)
{
    if (d < today || (d == today && hour >= rollover_hour))
    {
        return false;
    }
    return d > acked_day;
}

// undo lifts only the newest acknowledged day; earlier pending days stay done
static void ack_step_back(int32_t today, int hour)
{
    int32_t prev = -1;
    for (int i = 0; i < pickup_count && pickups[i].day < acked_day; i++) // sorted ascending
    {
        int32_t d = pickups[i].day;
        if (d > today || (d == today && hour < rollover_hour))
        {
            prev = d;
        }
    }
    if (prev >= 0)
    {
        ack_set(prev);
    }
    else
    {
        ack_clear();
    }
}
```

2. **Font.** Run this from the lvgl `scripts/built_in_font/` directory:
   `npx lv_font_conv@1.5.3 --font FontAwesome5-Solid+Brands+Regular.woff -r 0xF00C --size 48 --bpp 4 --no-compress --format lvgl -o src/fonts/symbol_check_48.c`.
   Then add `LV_FONT_DECLARE(symbol_check_48);` at `src/main.cpp:12`.
3. **`src/main.cpp` declarations, around `:19-32`:**
   - `enum OverlayState : uint8_t { OV_IDLE, OV_STAMP, OV_HOLD, OV_REVEAL };` plus `overlay_state` and `overlay_shown_ms`.
   - Object pointers `overlay`, `overlay_card`, `overlay_circle`, `overlay_check`, `overlay_types`, `overlay_hint`, and `overlay_timer`.
   - Forward declarations of `overlay_show(int32_t)` and `overlay_finish()`.
   - Constants: `ov_pop_delay_ms = 80` (lets the full-screen dialog-to-card frame finish first), `ov_pop_ms = 300`, `ov_min_ms = 400`, `ov_hold_ms = 1800`, `ov_fade_ms = 320`, `ov_stuck_ms = 10000`, `ov_circle = 76`.
4. **`update_display()`, top of the function:**

```cpp
    // The done card refers to the hero underneath; the reveal re-renders by itself.
    // Bounded so a stuck overlay can never freeze the calendar.
    if (overlay_state == OV_STAMP || overlay_state == OV_HOLD)
    {
        if (millis() - overlay_shown_ms < ov_stuck_ms)
        {
            Serial.println("render deferred: done overlay up");
            return;
        }
        overlay_finish();
    }
```

5. **`on_dialog_yes`:**
   - Start with `if (dialog_day < 0) { return; }`, commented with the PRESS_LOCK race.
   - Undo branch: `ack_step_back(...); dialog_close(); update_display(); return;`.
   - Acknowledge branch: `int32_t day = dialog_day; ack_set(day); dialog_close(); overlay_show(day);`. Hiding the dialog and showing the card in one handler means no intermediate frame. Do not call `update_display()` here.
6. **New section "done overlay" after `setup_dialog()`:**
   - `setup_overlay()` follows the `setup_dialog` pattern: backdrop with `on_overlay_clicked` on `LV_EVENT_CLICKED`, then hidden.
   - Card with `lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN)` and `lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER)`. Flex is used because the two lines wrap to one or two lines.
   - The card, row and circle each get `lv_obj_clear_flag(..., LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE)`, so taps reach the backdrop and the overshoot never shows a scrollbar.
   - The check label holds `LV_SYMBOL_OK` in `symbol_check_48`.
   - `overlay_timer = lv_timer_create(on_overlay_timer, ov_hold_ms, NULL); lv_timer_pause(overlay_timer);`.
   - Core sketch:

```cpp
// grows the circle around its centre; the glyph fades in once the circle can hold it
// (children clip to the parent's rectangle, so a half-grown circle would cut it square)
static void overlay_pop_exec(void *var, int32_t v)
{
    lv_obj_t *circle = (lv_obj_t *)var;
    lv_obj_set_size(circle, v, v);
    lv_obj_align(circle, LV_ALIGN_LEFT_MID, 8 + (ov_circle - v) / 2, 0);
    int32_t o = (v - 40) * 255 / 24;
    lv_obj_set_style_text_opa(overlay_check, (lv_opa_t)LV_CLAMP(0, o, 255), 0);
}

static void overlay_fade_exec(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0); // bg_opa: no layer, unlike opa
}

static void on_overlay_popped(lv_anim_t *a)
{
    if (overlay_state == OV_STAMP)
    {
        overlay_state = OV_HOLD;
    }
}

static void overlay_finish()
{
    lv_timer_pause(overlay_timer);
    lv_anim_delete(overlay_circle, NULL);
    lv_anim_delete(overlay, NULL); // safe from the fade's own completed_cb: already unlinked
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    overlay_state = OV_IDLE;
    Serial.printf("overlay idle after %lu ms\n", (unsigned long)(millis() - overlay_shown_ms));
}

static void overlay_reveal(const char *why)
{
    if (overlay_state != OV_STAMP && overlay_state != OV_HOLD)
    {
        return; // late timer after a forced close
    }
    lv_timer_pause(overlay_timer);
    lv_anim_delete(overlay_circle, overlay_pop_exec); // a tap can cut the pop short
    lv_obj_add_flag(overlay_card, LV_OBJ_FLAG_HIDDEN);
    overlay_state = OV_REVEAL; // before update_display(): the deferral no longer applies
    update_display();          // the next view is built in the frame the card leaves
    Serial.printf("overlay reveal (%s) after %lu ms\n", why, (unsigned long)(millis() - overlay_shown_ms));
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, overlay);
    lv_anim_set_exec_cb(&a, overlay_fade_exec);
    lv_anim_set_values(&a, LV_OPA_70, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, ov_fade_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, on_overlay_faded);
    lv_anim_start(&a);
}

// one-liners, defined before their first use: on_overlay_faded(a) -> overlay_finish(), on_overlay_timer(t) -> overlay_reveal("timer")
static void on_overlay_clicked(lv_event_t *e)
{
    // absorbs every tap; only one after ov_min_ms (not the bounce of "Ja") skips the hold
    if (millis() - overlay_shown_ms >= ov_min_ms)
    {
        overlay_reveal("tap");
    }
}
```

   - `overlay_show(day)` sets the types label from `types_due_on(day)`. It fills the hint from `next_hero_hint()` and hides the hint label when the string is empty. It then resets `bg_opa` to `LV_OPA_70`, clears `HIDDEN` on the card and backdrop, sets `OV_STAMP` and `overlay_shown_ms`, and starts the pop:
     - `var` = circle, values 20 → `ov_circle`;
     - delay `ov_pop_delay_ms`, duration `ov_pop_ms`;
     - `lv_anim_path_overshoot`, completed callback `on_overlay_popped`.
     `early_apply` defaults to 1 (`lv_anim.c:83`), so the 20 px circle shows from the first frame. It then calls `lv_timer_reset` and `lv_timer_resume` on `overlay_timer` and logs `lv_mem_monitor` free and biggest-block sizes.
   - `next_hero_hint(buf, n)` must be called after `ack_set`. It finds the first `pickups[i].day` with `pickup_pending(...)`. If `delta <= hero_from_days`, it writes `"Danach: %s %s"` with `types_due_on()` and "heute" / "morgen" / "übermorgen". Otherwise it writes an empty string.
7. **`setup_ui()`:** call `setup_overlay()` right after `setup_dialog()` (`src/main.cpp:670`). That keeps creation order, so the overlay draws above the dialog.
8. **Debug hooks, only under `#ifdef TRASH_DEBUG`:**
   - Count frames with `lv_display_add_event_cb(lv_display_get_default(), on_refr_ready, LV_EVENT_REFR_READY, NULL)` and print the count in `overlay_finish`.
   - Add `debug_serial()` to `loop()` after `next_fetch_epoch` is declared. It reads one character:
     - `h` adds Bio at today+1 and Rest at today+2, skipping duplicates, then calls `sort_pickups()` and `update_display()`;
     - `s` adds Gelb at today+1;
     - `t` adds Bio today (only before 10:00);
     - `c` runs `ack_clear()`;
     - `u` runs `update_display()`;
     - `w` sets `next_fetch_epoch = 0`.
9. **`README.md`:**
   - "Touch": describe the Erledigt card, about 2 s, a tap skips it; Ja marks that day and any earlier pending one; undo lifts the newest day only.
   - "Setup": add the font command.

## Resource budget

- **LVGL heap (`LV_MEM_SIZE` 64 KB):**
  - 8 persistent objects: about 1.5-2.5 KB with local styles. This is an estimate; log `lv_mem_monitor`.
  - Up to 2 running animations of about 100 B each, and 1 timer.
  - No layer buffers by design.
  - Peak load is about 25 hero + 8 dialog + 8 overlay + 3 screen objects, well under half the pool. Building once avoids create/delete fragmentation.
  - The draw buffer (38.4 KB) is outside the pool.
- **Redraw per frame:**
  - Dialog to card: one full screen, 153.6 KB of SPI, about 51 ms plus render.
  - Pop: at most 80×80 px, about 12.8 KB and 4.3 ms of SPI.
  - Hold: 0 px.
  - Reveal: about 5 full-screen frames, plus 1 when the backdrop is hidden.
- **Frame rate at 80 MHz, estimated:** the pop is limited by `LV_DEF_REFR_PERIOD` 33 ms plus the 10 ms loop, about 25-30 fps or 8-9 frames. The full-screen fade gets 10-14 fps because render and flush serialise on one buffer, so 4-5 visible steps.
- **Flash:**
  - `symbol_check_48.c` is about 0.9 KB bitmap plus 0.2 KB of tables. For comparison, `montserrat_de_32` has 17.9 KB of bitmap for 102 glyphs.
  - About 3 KB of code.
  - No `lv_conf.h` change and no new widget; the app slot is 3 MB.
  - `LV_FONT_MONTSERRAT_8` and `_16` are compiled in but not referenced in `src/`. They could be dropped to offset the new font after checking that the library does not use them.

## Risks and open questions

- No compile was possible here. Every LVGL name was checked against v9.2.2 sources: `lv_anim_set_completed_cb`, `lv_anim_delete`, `lv_anim_path_overshoot`, `lv_timer_pause/resume/reset`, `lv_mem_monitor`, `LV_EVENT_REFR_READY`, `lv_obj_set_style_text_opa/bg_opa`, `LV_SYMBOL_OK`. Flex has not been used in this code base before.
- The fade's 4-5 steps may look coarse on hardware. Setting `ov_fade_ms = 0` turns the reveal into a clean cut. Whether the dimmed hero stays readable through 70 % black on this TN panel is unknown; 60 % is the fallback.
- The FontAwesome glyph may sit a few pixels off-centre in the circle. Adjust with an `lv_obj_align` offset after a look on the device.
- The new acknowledgement semantics hide a date that a refresh inserts *before* an acknowledged day. That is a dataset correction inside a 2-day window, rare and accepted. Undo now steps back one day instead of clearing.
- The `ack_set` LittleFS write runs inside the click handler and delays the first card frame (unmeasured, guessed at 5-30 ms). The PRESS_LOCK race is based on code reading and has not been reproduced.
- A fetch window mid-overlay freezes the card for seconds. This is accepted, and `ov_stuck_ms` bounds the deferral.
- `ov_min_ms` = 400 is a guess about XPT2046 release bounce.
- Owner decisions:
  - Is a 1.8 s hold right?
  - Is the wording "Danach:" right?
  - Should the RGB LED flash green? `smartdisplay_led_set_rgb` exists, but the LED may sit on the back of the board.
  - Sound is out of scope: GPIO 26 needs an attached speaker.

## Verification

- Build with `PLATFORMIO_BUILD_FLAGS="-D TRASH_DEBUG" pio run -e esp32-2432S028R -t upload` and watch with `pio device monitor`. Expected log lines, in order:
  - `day N acknowledged`
  - `overlay show: day N, hint '...', lv heap free X, biggest Y`
  - `overlay reveal (timer|tap) after ~1800 ms`
  - `overlay idle after ~2150 ms, F frames`
  - `render deferred: done overlay up` (when applicable)
- Fake pickup days come from the serial commands `h`, `s` and `t`. They live only in RAM: `cache_save` runs only after a successful fetch (`src/trash_functions.h:903`), and a fetch replaces the current year. Do not send `w` with fakes on 30 or 31 December, when today+2 is next year and a December fetch could save it.
- **Single hero:** send `s`, tap, Ja. Check:
  - no flash of the bright hero between dialog and card;
  - the pop is smooth;
  - the card holds about 1.8 s, then fades to the list, with the label bottom-right.
- **Consecutive days and the bug regression:** send `c`, then `h`.
  1. Ja on Bio: the card shows the amber "Danach: Restmüll übermorgen" and then the Rest hero appears.
  2. Ja on Rest: no hint, then the list, label "Erledigt: <Tue>".
  3. Power-cycle: the list stays, and Bio must never return.
  4. Undo from the list brings back Rest only.
- **Taps and interruptions:**
  - A tap at about 0.2 s is ignored. A tap at about 1 s reveals at once (log `tap`).
  - `u` during the hold logs the deferral, and the dimmed hero stays the old one.
  - `w` right after Ja: the card stays through the window, then reveals with no crash.
  - Hold "Ja" past the 15 s timeout: no `day -1 acknowledged` line.
- **Resources:** run 10 overlay cycles and compare heap free and biggest block. They must not drift.
- **Glyph:** a rectangle instead of the check means the glyph is missing (`LV_USE_FONT_PLACEHOLDER 1`).
- **Dark room:** cover the light sensor. The backlight must stay on through the whole sequence.
