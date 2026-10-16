# Plan C: hero exit transition

The owner lifts the finger from "Ja". In that same refresh the dialog disappears and the hero shrinks to its core. The header ("Für morgen rausstellen:") becomes a green "Erledigt!", the date line and the compact rows vanish, and only the colored bin bar(s) stay where they were. After 100 ms a line in the bin name's own color (white, black on the yellow sack) draws through the name from left to right in 220 ms, like crossing an item off a list. With several bars the strikes follow each other 100 ms apart. After a 150 ms beat the bars speed up and leave over the right edge one after another (280 ms each, 70 ms apart). Then the green "Erledigt!" stands alone on black for 350 ms. At about 1.1 s (1.6 s with four bars) the next state arrives. A list or center message fades in over 300 ms, and the grey "Erledigt: Mo 05.10." receipt fades in bottom-right at the same time.

If another pickup falls within two days (Bio Monday acknowledged on Sunday, Rest on Tuesday), the next state is again a hero in the same place. It enters from the opposite side. "Übermorgen:" and its compact rows appear at once, the grey Restmüll bar slides in from the left edge (300 ms, slowing down), and the receipt fades in. Several cues mark it as a different pickup and not the old one coming back: the empty "Erledigt!" beat, the new header text and color, the entry from the side opposite the exit, and the receipt naming Monday. Taps during the whole sequence (1.5 to 2 s) only keep the backlight on. Acknowledging Tuesday later leads to the list and no longer brings Monday back (step 1).

## Current behaviour

- A tap on the screen with a hero showing opens the dialog for `shown_hero_day` (`src/main.cpp:553-556`), which is set only on the hero path (`main.cpp:354-358`).
- `on_dialog_yes` (`main.cpp:481-495`) calls `ack_set(dialog_day)`, which sets the single `int32_t acked_day` and rewrites `/ack.txt` (`src/trash_functions.h:728`, `:744-755`). It then calls `dialog_close()` (`main.cpp:464-469`) and `update_display()`.
- `update_display()` runs `lv_obj_clean(content)` (`main.cpp:288`) and skips pickups with `d == acked_day` (`:313`). So the hero **is** cleared in code. On screen it is not *visibly* cleared: hiding the dialog and the rebuild land in the same LVGL refresh, so the dimmed hero plus dialog jumps straight to the next state.
- **Bug: alternating heroes** (matches the owner's report that the hero stays no matter how often "Ja" is pressed). With Bio on Monday and Rest on Tuesday, and today being Sunday:
  1. Acknowledging Monday renders a Tuesday hero (two days ahead passes `hero_from_days` at `main.cpp:354`) in the same layout and position.
  2. Acknowledging Tuesday overwrites `acked_day`, so Monday is no longer skipped at `:313` and its hero returns.
  3. The two alternate forever. `ack_label` names only the last acknowledged day (`:383-391`), and the look-alike layout makes even the first "Ja" seem to do nothing.
- Other callers of `update_display()`: every 60 s (`main.cpp:775-779`), after each fetch window (`:772`), when time first becomes valid (`:744`), and at boot (`:703`).
  - The fetch window blocks `loop()` for seconds. Its `lv_refr_now()` calls (`:753`, `:758`) run LVGL's anim timer (`lv_refr.c:85`, 9.2.2) but no `lv_timer`.
  - `lv_tick_inc` runs only at the top of `loop()` (`:714`), so animations freeze during a fetch and jump to their end afterwards.
- Edge case:
  - Every child object gets `LV_OBJ_FLAG_PRESS_LOCK` (`lv_obj.c:498`), and on release `lv_indev.c` sends `CLICKED` to the pressed object without a hidden check.
  - So if the 15 s timeout closes the dialog while a finger rests on "Ja", `on_dialog_yes` runs with `dialog_day == -1`. It writes `-1` to `/ack.txt` and drops the acknowledgement.
- Edge case: a 60 s render can replace the hero while the dialog is open (10:00 rollover). "Ja" then acknowledges a day no longer shown, which is harmless (cleared at `main.cpp:302-305`).
- The undo path (`main.cpp:483-487`, `:560-563`) is separate and stays instant.

## Design

**Shape: "check off and sweep".** Only objects already on screen move: the header label and the bars that `render_hero` creates (`main.cpp:192-207`). The only new object is one 4 px `lv_obj` strike line per bar, created as a child of that bar. No new widget class, font, image, transform or layer.

Why animate the live objects and defer the rebuild instead of animating a detached copy:
- A copy needs a pointer swap of `content`, z-order fixes and a hidden new container. A guard at the top of `update_display()` does the same job in five lines.
- The sequencer is an `lv_timer`, not an anim completion callback. LVGL deletes an object's anims on destruction without calling `completed_cb` (`lv_obj.c:526`, `remove_anim` in `lv_anim.c`), so a sequencer tied to objects could stall.

**States** (`exit_phase`):

| State | Entered by | On screen | `update_display()` | Tap on screen |
|---|---|---|---|---|
| `EXIT_IDLE` | boot; `on_exit_timer` 2nd fire | normal | renders | as today |
| `EXIT_RUNNING` | "Ja" with that hero on screen | header + bars perform | **deferred** (returns, logs) | ignored |
| `EXIT_ENTER` | `on_exit_timer` 1st fire (`t_end`) | next state arriving | renders | ignored |

**Timeline** for n bars (all ms from "Ja"). `slide0 = 100 + 100(n-1) + 220 + 150`.

| Step | Start | Duration | Path |
|---|---|---|---|
| Header text to green "Erledigt!"; date and rows hidden | 0 | one frame (full-screen un-dim, as today) | |
| Strike on bar i, width 0 to bar width - 16 | 100 + 100i | 220 | ease out |
| Bar i slides from x 4 to x 320 | slide0 + 70i | 280 | ease in |
| Empty stage, "Erledigt!" alone | slide0 + 70(n-1) + 280 | 350 | |
| `t_end`: rebuild, then entrance (new hero bars from x -320, stagger 60 ms; otherwise `content` fade-in; receipt fade-in) | n=1: 1100, n=4: 1610 | 300 | ease out |
| `EXIT_IDLE` | t_end + 350 | | |

The 100 ms lead absorbs the LittleFS write and the full-screen un-dim frame, so the strike starts smoothly.

**Interleaving:**
- `ack_set()` runs first, before anything moves, so a reset mid-animation keeps the mark.
- An `update_display()` call during `EXIT_RUNNING` returns at once. `on_exit_timer` renders at `t_end` with whatever is current then (rollover, fresh data, expired ack).
- `loop()` does not start a fetch window during the exit (at most 2 s of delay), so the exit never freezes mid-motion.
- A tap during `EXIT_RUNNING`/`EXIT_ENTER` still wakes the backlight (`on_touch_pressed` is an input-device event) but opens nothing. A bounce or a second tap on "Ja" must neither cut the exit short nor open the next hero's dialog.
- If the dialog's day is no longer the hero on screen, the old instant re-render runs.

**Optional cues:**
- **RGB LED (GPIO 4/16/17, active low, on/off only):** green from "Ja" to `EXIT_IDLE`. Costs about 50 B of flash and 0 B of RAM. On the boards I know, the LED sits on the back of the PCB, so visibility depends on the mounting. Behind `ACK_LED_CUE`, off by default.
- **Speaker (GPIO 26):** not recommended. The CYD normally has no speaker plugged in, and a beep from a hallway display at 7:00 is a nuisance. Core 2.0.17 `tone()` permanently allocates a 3500 B task stack plus a 128-entry queue on first use; it uses LEDC channel 0, while the backlight is on channel 15, so they don't clash. `ledcWriteTone` plus a one-shot `lv_timer` would avoid the task.
- **Checkmark (✓) before "Erledigt!":** not needed, since the strike is the checkmark. If wanted, regenerate `montserrat_de_20` with FontAwesome U+F00C added (about 0.2 KB).

## Implementation steps

1. **Fix the alternation** (`src/main.cpp` `update_display`; update the comment at `src/trash_functions.h:725-727`). The hero always shows the earliest day not yet acknowledged, so acknowledged days always form a prefix of the upcoming days, and "every day up to `acked_day`" equals the set the owner confirmed. Treat `acked_day` as a high-water mark: same `/ack.txt` format, same 10:00 expiry via `ack_active`.
   - Rejected: acknowledging every day in the hero window at once, because that would hide Tuesday before its bin is out.
   - Alternative for per-day undo: a 3-slot array in `/ack.txt`, each slot expiring at its own rollover (about 30 more lines).
   ```cpp
   // a hero tap can only acknowledge a day within hero_from_days; anything later is a
   // corrupt file and must not hide the calendar under the high-water rule below
   if (acked_day > today + hero_from_days)
   {
       ack_clear();
   }
   ...
       if (d < today || (d == today && tmnow.tm_hour >= rollover_hour) || d <= acked_day)
   ```
2. **Statics and constants** (`main.cpp`, top). Keep them in `main.cpp`, not `Config.h`: the owner's untracked `Config.h` would not have new constants.
   ```cpp
   enum HeroExitPhase : uint8_t
   {
       EXIT_IDLE,
       EXIT_RUNNING, // old hero performs; update_display is deferred, taps ignored
       EXIT_ENTER    // next state arriving; renders allowed, taps still ignored
   };
   static HeroExitPhase exit_phase = EXIT_IDLE;
   static lv_timer_t *exit_timer;        // sequencer: an lv_timer survives any lv_obj_clean
   static lv_obj_t *hero_header = NULL;  // valid until the next lv_obj_clean(content)
   static lv_obj_t *hero_bars[TYPE_COUNT];
   static int hero_bar_count = 0;
   static uint32_t exit_start_ms, exit_frames, exit_max_frame_ms, exit_refr_start_ms;
   static const uint32_t exit_lead_ms = 100, exit_strike_ms = 220, exit_strike_gap_ms = 100,
                         exit_beat_ms = 150, exit_slide_ms = 280, exit_slide_gap_ms = 70,
                         exit_hold_ms = 350, enter_ms = 300, enter_gap_ms = 60;
   ```
3. **`update_display` / `render_hero`.** Add the guard below and reset the records after the clean. In `render_hero`, store `hero_header = make_header(...)` in each branch (it already returns the label, `main.cpp:145-153`) and add `hero_bars[hero_bar_count++] = bar;` after each bar.
   ```cpp
   if (exit_phase == EXIT_RUNNING)
   {
       Serial.println("update_display deferred: hero exit running");
       return; // on_exit_timer renders at t_end with whatever is current then
   }
   lv_obj_clean(content);
   shown_hero_day = -1;
   hero_header = NULL; // the objects died with the clean
   hero_bar_count = 0;
   ```
4. **Animation helpers** (new section "hero exit", after `update_display`):
   - `anim_set_x(void *obj, int32_t v)` and `anim_set_width(...)`: thin wrappers around `lv_obj_set_x` / `lv_obj_set_width`, so C++ needs no function-pointer casts to `lv_anim_exec_xcb_t`.
   - `start_anim(obj, cb, from, to, duration, delay, path)`: the usual `lv_anim_init`, `lv_anim_set_var/exec_cb/values/duration/delay/path_cb`, `lv_anim_start` sequence. It keeps the `lv_anim_init` default `early_apply = 1` on purpose: `from` is written at once, so a delayed bar never flashes at its end position.
5. **`hero_exit_start(int32_t day)`** (new). Read geometry from the local style values: coordinates can be stale if a render ran in this `lv_timer_handler` pass.
   ```cpp
   static bool hero_exit_start(int32_t day)
   {
       if (shown_hero_day != day || hero_header == NULL || hero_bar_count == 0)
       {
           return false; // a render replaced the hero while the dialog was open
       }
       shown_hero_day = -1;
       exit_phase = EXIT_RUNNING;
       uint32_t n = lv_obj_get_child_count(content);
       for (uint32_t i = 0; i < n; i++) // date line and compact rows leave with the dialog
       {
           lv_obj_add_flag(lv_obj_get_child(content, i), LV_OBJ_FLAG_HIDDEN);
       }
       lv_obj_clear_flag(hero_header, LV_OBJ_FLAG_HIDDEN);
       lv_label_set_text(hero_header, "Erledigt!"); // ASCII only: montserrat_de_20 has it
       lv_obj_set_style_text_color(hero_header, lv_color_hex(0x66BB6A), 0);

       uint32_t slide0 = exit_lead_ms + (hero_bar_count - 1) * exit_strike_gap_ms
                         + exit_strike_ms + exit_beat_ms;
       for (int i = 0; i < hero_bar_count; i++)
       {
           lv_obj_t *bar = hero_bars[i];
           lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
           lv_obj_t *strike = lv_obj_create(bar); // child, so it leaves with its bar
           lv_obj_set_size(strike, 0, 4);
           lv_obj_set_pos(strike, 8, lv_obj_get_style_height(bar, 0) / 2 - 2);
           // the bin name's color: white, black on the yellow sack
           lv_obj_set_style_bg_color(strike,
               lv_obj_get_style_text_color(lv_obj_get_child(bar, 0), LV_PART_MAIN), 0);
           lv_obj_set_style_border_width(strike, 0, 0);
           lv_obj_set_style_radius(strike, 2, 0);
           lv_obj_clear_flag(strike, LV_OBJ_FLAG_CLICKABLE);
           start_anim(strike, anim_set_width, 0, lv_obj_get_style_width(bar, 0) - 16,
                      exit_strike_ms, exit_lead_ms + i * exit_strike_gap_ms, lv_anim_path_ease_out);
           start_anim(bar, anim_set_x, lv_obj_get_style_x(bar, 0), 320, exit_slide_ms,
                      slide0 + i * exit_slide_gap_ms, lv_anim_path_ease_in);
       }
       uint32_t t_end = slide0 + (hero_bar_count - 1) * exit_slide_gap_ms + exit_slide_ms + exit_hold_ms;
       lv_timer_set_period(exit_timer, t_end);
       lv_timer_reset(exit_timer);
       lv_timer_resume(exit_timer);
       exit_start_ms = millis();
       exit_frames = exit_max_frame_ms = 0;
       Serial.printf("hero exit: day %ld, %d bars, rebuild in %lu ms\n", (long)day, hero_bar_count, (unsigned long)t_end);
       return true;
   }
   ```
6. **`on_exit_timer`** (new). It fires twice: once to rebuild, once to finish.
   ```cpp
   static void on_exit_timer(lv_timer_t *t)
   {
       if (exit_phase == EXIT_RUNNING)
       {
           Serial.printf("hero exit: rebuild after %lu ms, %lu frames, slowest %lu ms\n",
                         millis() - exit_start_ms, (unsigned long)exit_frames, (unsigned long)exit_max_frame_ms);
           exit_phase = EXIT_ENTER; // renders allowed again
           update_display();        // deletes the old bars; their anims have finished
           if (shown_hero_day >= 0)
           {
               // another pickup within hero_from_days: enter from the side opposite to
               // the exit so it cannot read as the old hero coming back
               for (int i = 0; i < hero_bar_count; i++)
               {
                   start_anim(hero_bars[i], anim_set_x, -320, lv_obj_get_style_x(hero_bars[i], 0),
                              enter_ms, i * enter_gap_ms, lv_anim_path_ease_out);
               }
           }
           else
           {
               lv_obj_fade_in(content, enter_ms, 0); // `opa`, not `opa_layered`: no layer buffer
           }
           if (!lv_obj_has_flag(ack_label, LV_OBJ_FLAG_HIDDEN))
           {
               lv_obj_fade_in(ack_label, enter_ms, 0); // the receipt
           }
           lv_timer_set_period(exit_timer, enter_ms + 50);
           lv_timer_reset(exit_timer);
           return;
       }
       exit_phase = EXIT_IDLE;
       lv_timer_pause(exit_timer);
       // log lv_mem_monitor(): "hero exit: done, lvgl mem <used_pct>% used, max <max_used> B, frag <frag_pct>%"
   }
   ```
7. **`on_dialog_yes`**. Copy `dialog_day` first and return if it is negative (the timeout race). Call `dialog_close()`. Keep the undo branch as it is (`ack_clear(); update_display();`). The acknowledge branch becomes: `ack_set(day); if (!hero_exit_start(day)) { update_display(); }`.
8. **`on_screen_clicked`**: first statement `if (exit_phase != EXIT_IDLE) { return; }`, with a comment giving the bounce and second-tap reason.
9. **`setup_dialog` / `setup_ui`**:
   - Create the sequencer next to `dialog_timer`: `exit_timer = lv_timer_create(on_exit_timer, 1000, NULL); lv_timer_pause(exit_timer);`.
   - Register one display event callback (`lv_display_add_event_cb`) for `LV_EVENT_REFR_START` (store `millis()`) and `LV_EVENT_RENDER_READY`, which fires only when something was drawn: count `exit_frames`, track `exit_max_frame_ms`.
   - The callback does nothing while `EXIT_IDLE` and during the first 100 ms, so the un-dim frame doesn't count.
10. **`loop()`**: the fetch condition becomes `time_valid && now >= next_fetch_epoch && exit_phase == EXIT_IDLE`. Comment: a fetch blocks the loop for seconds and would freeze the exit mid-motion, so it waits under 2 s.
11. **Optional LED.** `#ifndef ACK_LED_CUE` / `#define ACK_LED_CUE 0`. Under `#if defined(BOARD_HAS_RGB_LED) && ACK_LED_CUE`: `smartdisplay_led_set_rgb(false, true, false)` in `hero_exit_start`, and `(false, false, false)` where `EXIT_IDLE` is set.
12. **Test hook** (`main.cpp`, `#ifdef HERO_TEST`). `test_poll_serial()` is called from `loop()` after `lv_timer_handler()` and reads one byte from `Serial`.
    - It does nothing until time is valid.
    - `test_inject(day, type)` appends to `pickups[]` while `pickup_count < MAX_PICKUPS`, then `sort_pickups()` (`trash_functions.h:571`) and `update_display()` run.
    - RAM only, never `cache_save()`: the next fetch or a reboot restores the real calendar.
    - Commands (`today` from `today_day(NULL)`):

      | Key | Effect |
      |---|---|
      | `0` | Bio today ("HEUTE:" before 10:00) |
      | `1` | Bio tomorrow (one bar) |
      | `2` | Bio tomorrow + Rest the day after (adjacent heroes) |
      | `4` | all four types tomorrow (four bars) |
      | `c` | `ack_clear()` |
      | `u` | plain re-render; during the exit it must log "deferred" |
13. **`README.md`**:
    - "Display behavior": describe the check-off-and-sweep, and that acknowledging a day also covers earlier days still on screen.
    - "Touch": taps are ignored for about 2 s after "Ja".
    - "Setup": mention `HERO_TEST`.

## Resource budget

- **LVGL heap (`LV_MEM_SIZE` 64 KB):**
  - Today, estimated (not measured): about 35 objects (screen, `content`, 3 labels, 8 dialog objects, up to 17 hero or list objects), roughly 10-15 KB.
  - The exit adds up to 4 strike objects (about 200 B each with local styles and theme refs) and up to 8 `lv_anim_t` (88 B plus list node and allocator header, about 100 B each).
  - Peak is about +2 KB; everything except the permanent `exit_timer` (about 40 B) is freed at the rebuild. The step 6 log reports `max_used` and `frag_pct`.
- **No layer buffers:** no transform, no `opa_layered`, no blend mode, so `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` (24 KB) stays unused. The fade uses `opa`, applied per draw call (`lv_obj_get_style_opa_recursive` in `lv_obj_draw.c`).
- **Draw buffer unchanged:** one buffer of 19,200 px (38.4 KB). Verified: `lvgl_panel_ili9341_spi.c` at the pinned commit passes `NULL` as the second buffer, so rendering and SPI never overlap, and its flush callback byte-swaps every pixel on the CPU.
- **Static RAM:** about 50 B.
- **Redraw area and frame rate.** SPI at 24 MHz with 16 bit color is 0.67 µs per pixel. Render cost at 80 MHz is an estimate. The ceiling is 30 fps (`LV_DEF_REFR_PERIOD` 33 ms plus `delay(10)` in `loop()`).

  | Phase | Dirty area per frame | Buffer chunks | SPI time | Expected |
  |---|---|---|---|---|
  | Un-dim at T0 | 320x240 = 76.8 kpx | 4 | 51 ms | one frame, same as today |
  | Strike | 296x4 px per bar | 1 | under 1 ms | 30 fps |
  | Slide, one 54 px bar | at most 320x54 = 17.3 kpx | 1 | 11.5 ms | about 30 fps |
  | Slide, four 28 px bars overlapping | at most 320x136 = 43.5 kpx | 3 | 29 ms | 20-25 fps |
  | Rebuild at `t_end` | 320x222 = 71 kpx | 4 | 47 ms | one frame, same as today |
  | Hero entrance | like the slide | | | 25-30 fps |
  | Fade-in | 71 kpx | 4 | 47 ms | about 15 fps, 4-5 steps |

- **Flash:**
  - About 1.5 KB of code (estimate). 0 B of assets: no new font, image or widget class; `lv_anim` and `lv_timer` are already linked. `HERO_TEST` adds code to test builds only.
  - Rejected option: the built-in `lv_font_montserrat_16` checkmark glyph would cost about 13 KB (11.1 KB of bitmap). The font is enabled in `lv_conf.h` but nothing in `src/` references it, so it is probably not linked today.
- **Power:** at most 2 s of drawing at 80 MHz, not measurable. No CPU boost.

## Risks and open questions

- **Unverified without hardware:**
  - Frame rates, render cost at 80 MHz, and tearing. The ILI9341 has no TE sync here, so a fast horizontal slide may shear by one band.
  - Fallback if the four-bar slide logs a slowest frame over 50 ms: wrap the exit in `setCpuFrequencyMhz(240)` / `(80)` (2 lines; APB stays at 80 MHz, so SPI and LEDC are unaffected).
- **Strike height:** the strike passes through the label box's center. With montserrat 32 that may sit a few pixels above the middle of the lowercase letters; adjust on the device (±3 px).
- **Fade-in steps:** the fade-in has 4-5 visible steps. If that looks steppy, replace it with a cut (delete one line).
- **High-water ack** (step 1):
  - "Markierung aufheben?" lifts every acknowledged day, so Monday's hero may return first even though the dialog names Tuesday.
  - A data correction that adds a pickup before `acked_day` stays hidden until the mark expires.
  - The owner should accept this or choose the 3-slot variant.
- **Speed:** total time to the next state is 1.1-1.6 s. The constants sit in one block; is the 350 ms hold right when acknowledging two days in a row?
- **Celebration level:** the request mentioned hearts, confetti or a big overlay. This plan is deliberately restrained (green confirmation, strike, sweep). It can be combined with plan B's text if the owner wants more.
- **LED:** visibility depends on the mounting. **Fetch delay:** a fetch window that comes due during the exit waits under 2 s.
- **Touch mapping:** the hardware check on 2026-10-16 showed `touch_read_rotated` duplicating the rotation LVGL 9.2.2 applies itself; the wrapper is removed. Re-run the corner check after flashing. The exit adds no touch targets; check the "touch x,y" log lines while testing.
- **API verification:** I checked against the LVGL v9.2.2 sources on GitHub:
  - anim: `lv_anim_init` (`early_apply = 1`), the `lv_anim_set_*` calls, `lv_anim_path_ease_in/out`
  - timer: `lv_timer_set_period/reset/pause/resume`
  - object and style: `lv_obj_fade_in`, `lv_obj_get_style_x/width/height/text_color`, `lv_obj_get_child(_count)`
  - display and memory: `lv_display_add_event_cb`, `LV_EVENT_REFR_START/RENDER_READY`, `lv_mem_monitor`
  - semantics: anim deletion on object destruction, and `lv_refr_now` running the anim timer
  - Nothing has been compiled (PlatformIO is not installed here).

## Verification

1. **Build:** `pio run -e esp32-2432S028R`. Compare the RAM/Flash summary lines with a build of the current commit; expect flash to grow by under 3 KB and static RAM by under 100 B.
2. **Test build:** `PLATFORMIO_BUILD_FLAGS="-D HERO_TEST" pio run -e esp32-2432S028R -t upload && pio device monitor`. Type the single-key commands from step 12; time must be valid first. Finish every session with `c`: under the high-water rule, a test acknowledgement for a fake day hides real pickups up to that day.
3. **Scenarios and what to observe:**
   - **`1`, then tap and "Ja":**
     - Logs `touch x,y`, `day N acknowledged`, `hero exit: day N, 1 bars, rebuild in 1100 ms`, `rebuild after ~1100 ms, F frames, slowest S ms`, `done, lvgl mem ...`.
     - On screen: green header, strike through the name, bar leaving to the right, an empty beat, the list fading in, the receipt bottom-right.
     - No flash of the old bar at `t_end`, no leftover strike.
   - **`2` (adjacent case):**
     - Acknowledge Monday: the Tuesday hero slides in from the left.
     - Acknowledge Tuesday: the list appears, Monday does **not** return, and the receipt reads Tuesday.
     - Reboot: `ack loaded: day <Tuesday>` and still no Monday hero.
   - **`4` (four bars):** staggered strikes and slides. The slowest frame should be at most 50 ms; otherwise apply the CPU boost fallback.
   - **Tap during the exit:** `touch x,y` is logged, no dialog opens, and the backlight stays on in a dark room (cover the light sensor).
   - **`u` during the exit:** logs `update_display deferred: hero exit running`, and the exit completes normally.
   - **Power cut at about 0.5 s into the exit:** after boot, `ack loaded` shows the day and its hero is gone.
   - **`0` before 10:00:** the "HEUTE:" hero takes the same path. Inject at 09:59 and acknowledge after 10:00: the exit runs, then the rebuild clears the expired ack, so no receipt appears.
   - **Undo:** "Markierung aufheben?" then "Ja" stays instant, as today.
4. **Optional:** a `HERO_TEST` build with `-D ACK_LED_CUE=1` to judge LED visibility in the final mounting.
