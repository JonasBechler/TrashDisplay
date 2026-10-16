# Plan A: particle celebration

The owner taps the hero and presses "Ja". On the next frame, about 70 ms later (a full-screen redraw over 24 MHz SPI), the dialog and the whole hero are gone. The screen is black except for a green "Erledigt!" with the bin names below it ("Biomüll"), the grey "Erledigt: Mo 05.10." line bottom right, and 24 particles placed where the colored bars were: 16 small confetti pieces (lightened bin colors plus green and white), 4 red hearts and 4 gold stars. They hold still for 120 ms, as if the bars had just shattered. Then they burst outward and upward. The confetti flips (its width swings between 1 and 8 px), the hearts and stars sway, and everything falls under gravity across the text and off the bottom edge.

After 1.5 s of flight the particles are gone, with any stragglers faded out over the last 300 ms. About 1.6 s after the tap the display shows the next state in one redraw: either the list, or, if another pickup is within two days, that day's hero ("Übermorgen: Restmüll"), still with "Erledigt: Mo 05.10." bottom right. Taps during the burst do nothing except keep the backlight on. Acknowledging that next hero plays the same burst in its colors and then moves on. The earlier hero does not come back. Today it does, see below.

## Current behaviour

- On "Ja", `on_dialog_yes` (src/main.cpp:481-495) calls `ack_set(dialog_day)`, then `dialog_close()` (hides the backdrop, pauses the timeout, sets `dialog_day = -1`; :464-469), then `update_display()`.
- `ack_set` (src/trash_functions.h:744-755) overwrites the single `int32_t acked_day` (:728) and rewrites `/ack.txt`.
- `update_display` (src/main.cpp:286-396) runs `lv_obj_clean(content)` (:288), resets `shown_hero_day` (:289), skips pickups with `d == acked_day` (:313), then renders a hero when `next_day - today <= hero_from_days` (:354-358) and the list otherwise (:361). It sets `ack_label` at :382-395.
- **Single pickup day:** the code does clear the hero. It happens in the same LVGL pass that hides the dialog, so the user never sees the hero undimmed again. The dimmed hero behind the card is replaced by the next view in one redraw of about 70-100 ms, with no transition. If the next view is another hero, it has the same layout (header, full-width bar), so the change is easy to miss.
- **Bug, verified by reading the code; it matches the owner's report that the hero persists no matter how often "Ja" is pressed.** Bio on Monday, Rest on Tuesday, today Sunday:
  - Acknowledging Monday skips it at :313, and Tuesday renders as "Übermorgen: Restmüll" (:354, within `hero_from_days`).
  - Acknowledging Tuesday overwrites `acked_day` (trash_functions.h:746), Monday is no longer skipped, and "Für morgen rausstellen: Biomüll" comes back. The two heroes alternate forever while `ack_label` names the other day. The same happens on Monday before 10:00 with "HEUTE:".
- **Second cause to rule out:** README "Touch" notes that a left-right mirrored touch mapping swaps Ja and Nein, so "Ja" would run `on_dialog_no` and log nothing. Hardware check on 2026-10-16: `touch_read_rotated` applied the 270° rotation that LVGL 9.2.2 already applies in `indev_pointer_proc`, so taps were rotated twice and "Ja" landed on the card text. The wrapper has since been removed from `setup_touch`; the corner check in README "Touch" confirms the fix. The serial line `day N acknowledged` (:490) tells the two causes apart.
- **Re-render points:** at boot (:703), when time becomes valid (:744), after every fetch window (:772), and every 60 s (:775-779). A fetch window blocks `loop()` for seconds (WiFi, TLS) and only calls `lv_refr_now` (:753, :758), so any `lv_anim` freezes during it.
- **Taps:** screen `CLICKED` goes to `on_screen_clicked` (:547-564). The indev `PRESSED` hook (:440-446) keeps the backlight on for 20 s.
- **Minor:** `ack_load` uses `File::parseInt` (trash_functions.h:738) on a file written without a trailing newline. Stream's `parseInt` then likely waits out its 1 s timeout at EOF on every boot that has an acknowledgement (unverified).
- **Minor:** include/lv_conf.h:394 also enables `LV_FONT_MONTSERRAT_16`, not only 8 and 12. main.cpp never references it.

## Design

### How the particles are drawn

The particles are not LVGL objects. One transparent full-screen object on `lv_layer_top()` (the *overlay*) draws them all from a static array in its `LV_EVENT_DRAW_MAIN` callback:
- confetti with `lv_draw_rect`;
- hearts and stars as single glyphs with `lv_draw_label`.

One `lv_anim` supplies the time. Each step invalidates one area per particle: the union of its previous and new box.

| Option | Cost | Verdict |
|---|---|---|
| One `lv_obj`/label per particle | About 150-250 B of LV_MEM each, plus a style and layout pass per move. Each move invalidates both old and new coords (lv_obj_pos.c:717, :746), so 24 particles need about 48 areas, more than the 32-slot list (`LV_INV_BUF_SIZE`, lv_display_private.h:29). On overflow the whole screen is redrawn (lv_refr.c:320-322), at 60-90 ms per frame. | rejected |
| Custom draw on one overlay | 1 object, about 0.8 KB .bss, at most 24 areas per frame | **chosen** |
| Hearts and stars: new 2-glyph font (FontAwesome U+F004, U+F005) | Generated here with lv_font_conv 1.5.3 at 22 px, 4 bpp: 306 B of bitmap (heart 24×21, star 25×24, line height 24), about 0.5 KB in total | **chosen** |
| `LV_SYMBOL_*` in the built-in fonts | No heart or star (lv_symbol_def.h). Only `LV_SYMBOL_OK` would fit, and montserrat_16 alone has 11 KB of bitmaps. | rejected |
| Enable canvas, image or line | A full-screen RGB565 canvas needs 150 KB. Images need assets and a decoder. Neither adds anything over glyphs. | rejected |

There is no rotation, scaling, `opa_layered` or `lv_obj_fade_*`. Transforms render through layers allocated in LV_MEM, and fading `content` would redraw the full screen at every step. The confetti's flipping is faked by changing its width.

### Motion

The position at `t` ms into the flight is closed-form: `x = x0 + vx·t`, `y = y0 + vy·t + g·t²/2`, with `g = 600 px/s²`.
- **Start point:** random, inside the hero bar rectangles recorded when the hero was rendered, assigned round-robin per bar.
- **Speeds:** `vx = (x0 − 160)/2 ± 60 px/s`, so pieces move away from the middle and the burst opens up; `vy = −160 … −320 px/s`. The slowest pieces leave the bottom by about 1.1 s. The fastest peak about 90 px above their start, briefly off the top edge, and are near the bottom at 1.5 s.
- **Shapes and opacity:** confetti 6 px tall, width `1 + 7·|sin(0.75°/ms·t + phase)|`; glyphs in a 28×26 box with ±6 px sway; opacity 255 until t = 1200 ms, then linear to 0 at t = 1500 ms.

### State machine

| State | Entered | Screen | Exit |
|---|---|---|---|
| IDLE | boot, end of a burst | as today | "Ja" on a hero dialog → HOLD |
| HOLD (0-120 ms) | `celebration_start(day)` | done message, particles at their start points; the first (full-screen) frame is drawn here | anim delay over → FLIGHT |
| FLIGHT (120-1620 ms) | anim running | particles moving; the last 300 ms fade | anim completed → IDLE via `celebration_finish()` |
| (any) | | | watchdog: 4000 ms after start → `celebration_finish()` |

- `celebration_running()` is simply `celeb_overlay != NULL`; that one pointer covers both HOLD and FLIGHT.
- **When the hero content is rebuilt:** the hero is removed at t = 0, in the same pass as the dialog hide; the done message sits under the burst; the next view is built after it, at t ≈ 1620 ms. Reasons: a following hero should not appear under confetti in the previous day's colors; particles over black are the cheapest to redraw; and the acknowledgement is on flash before t = 0, so a reset during the burst lands on the correct next view.

### Interactions

- **Periodic `update_display()` from `loop()`:** skipped while a burst runs. `celebration_finish()` renders and stamps `last_render_ms`, so nothing is lost. If some other path still calls it mid-burst, it swaps the done message for the next view under the confetti. The overlay is independent of `content` and keeps running. That is harmless.
- **Fetch window:** deferred while a burst runs (at most 1.6 s late). Otherwise it would freeze the anim for seconds. The watchdog covers anything that still starves `lv_timer_handler`.
- **Taps during the burst:** the overlay is clickable and sits above the screen, so it absorbs them; no dialog opens and the burst is not skipped. The `PRESSED` hook still keeps the backlight on and logs `touch x,y`. The overlay has no styles, so a press causes no redraw. A finger still down when the overlay is deleted produces no click on the new view: deletion waits for the release (`obj_indev_reset`, lv_obj_tree.c:490-501).
- **Adjacent hero (Sunday: Bio Monday, Rest Tuesday):**
  - Acknowledge Monday: black screen, "Erledigt!" / "Biomüll", brown-green-white confetti, hearts and stars.
  - Then "Übermorgen: Restmüll" with a grey bar and "Erledigt: Mo 05.10." bottom right. The different header, bar color and the named done message show that this is the next task, not the old one coming back.
  - Acknowledge Tuesday: grey burst, then the list with "Erledigt: Di 06.10.". Monday stays acknowledged (step 1).
- **Undo ("Markierung aufheben?"):** no celebration. Lifting an acknowledgement re-renders immediately, as today.
- **Dark room:** the "Ja" press has just set `touch_wake_until_ms`, so the burst is lit.

## Implementation steps

1. **Fix the alternating heroes.**
   - In src/trash_functions.h:723-767, replace `acked_day` with a small sorted set. Each day expires at its own 10:00 rollover, and `/ack.txt` holds one day per line (an old one-number file still loads).
   - Call sites in src/main.cpp: :302-305 → `ack_expire(today, tmnow.tm_hour)`; :313 → `ack_has(d)`; :383-395 → `ack_latest()`; :486 → `ack_remove(dialog_day)`; :562 → `dialog_open(ack_latest(), true)`.
   - Rejected: acknowledging every pickup in the hero window at once. It would hide Tuesday's Restmüll reminder on Monday evening, when it is still needed.
   - Minimal fallback: skip `d <= acked_day` at :313. This works because the hero only ever offers the earliest open day. But undo then also brings back earlier days the user did not ask for.
   ```cpp
   // Consecutive pickup days (Bio Monday, Rest Tuesday) share the hero window;
   // one slot made the two heroes alternate. Kept ascending.
   #define MAX_ACKS 4 // the hero window spans three days
   static int32_t acked_days[MAX_ACKS];
   static int acked_count = 0;
   // ack_has(day), ack_latest() (-1 if none), ack_set(day) (insert sorted, drop the oldest
   // when full), ack_remove(day), ack_expire(today, hour), ack_active(today, hour); every
   // mutation ends in ack_save() (one day per line, file removed when the set is empty).
   // ack_load(): f.read() into a 64 B buffer, strtol() in a loop, log "ack loaded: %d day(s)".
   // Not parseInt: it waits out Stream's 1 s timeout at EOF.
   ```
2. **Add the symbol font.** Generate `src/fonts/celebrate_22.c` with the same tool and flags as the umlaut fonts, and add the command to README "Setup":
   `npx lv_font_conv@1.5.3 --font FontAwesome5-Solid+Brands+Regular.woff -r 0xF004,0xF005 --size 22 --bpp 4 --no-compress --format lvgl --lv-font-name celebrate_22 -o src/fonts/celebrate_22.c`
   The woff ships with lvgl in `scripts/built_in_font/`. Then add `LV_FONT_DECLARE(celebrate_22);` to main.cpp:10-12.
3. **Record the hero bar geometry and factor out the ack label.**
   - In `render_hero` (main.cpp:192-200), after `lv_obj_set_pos(bar, 4, y)`, store `lv_area_set(&hero_bar_area[hero_bar_count], 4, y, 315, y + bar_h - 1); hero_bar_type[hero_bar_count++] = t;`. Store geometry, not pointers, because `lv_obj_clean` deletes the bars.
   - Reset `hero_bar_count = 0` next to :289.
   - Move :382-395 into `static void update_ack_label()` so the burst can show the new "Erledigt:" line immediately.
   - Add a prototype for `types_due_on` near :32. The done message uses it, and it is defined later, at :498.
4. **Add the celebration section** to main.cpp between `dialog_close()` and `on_dialog_yes`. The non-obvious parts:
   ```cpp
   static const uint32_t celeb_hold_ms = 120;   // covers the full-screen first frame
   static const uint32_t celeb_flight_ms = 1500;
   static const uint32_t celeb_fade_ms = 300;   // last part of the flight
   static const uint32_t celeb_watchdog_ms = 4000; // lv_timer_handler starved or the anim lost
   static const int32_t celeb_gravity = 600;    // px/s^2
   static const bool celeb_boost_cpu = true;    // 240 MHz; APB, and so the SPI clock, stays 80 MHz
   #define CELEB_CONFETTI 16
   #define CELEB_PARTICLES (CELEB_CONFETTI + 8) // + 4 hearts, 4 stars
   struct Particle
   {
       int16_t x0, y0, vx, vy, phase; // px, px/s, degrees
       uint8_t kind;                  // PK_CONFETTI, PK_HEART, PK_STAR
       lv_color_t color;
       lv_area_t box;                 // drawn as is, and unioned into the next invalidation
   };
   static Particle particles[CELEB_PARTICLES];
   static lv_obj_t *celeb_overlay = NULL; // non-NULL exactly while a burst runs
   static int32_t celeb_t = 0;            // ms into the flight
   static uint32_t celeb_start_ms = 0;    // for the watchdog in loop()

   static void particle_box(const Particle *p, int32_t t, lv_area_t *out)
   {
       int32_t x = p->x0 + p->vx * t / 1000;
       int32_t y = p->y0 + p->vy * t / 1000 + (celeb_gravity * t / 1000) * t / 2000;
       if (p->kind == PK_CONFETTI)
       {
           // a flat piece turning over: the visible width swings between 1 and 8 px
           int32_t w = 1 + LV_ABS(lv_trigo_sin((int16_t)((t * 3 / 4 + p->phase) % 360))) * 7 / LV_TRIGO_SIN_MAX;
           lv_area_set(out, x - w / 2, y, x - w / 2 + w - 1, y + 5);
           return;
       }
       x += lv_trigo_sin((int16_t)((t / 3 + p->phase) % 360)) * 6 / LV_TRIGO_SIN_MAX;
       // 28x26 holds the 25x24 star with margin; a pixel outside the box would leave a trail
       lv_area_set(out, x - 14, y - 13, x + 13, y + 12);
   }

   // anim exec: one invalid area per particle keeps a frame at <= 24 of LVGL's 32 slots.
   // area_union/areas_overlap are local helpers: lv_area_join/_intersect are private in 9.2.2
   static void celebration_step(void *var, int32_t t)
   {
       celeb_t = t;
       for (int i = 0; i < CELEB_PARTICLES; i++)
       {
           lv_area_t now, u;
           particle_box(&particles[i], t, &now);
           area_union(&u, &particles[i].box, &now);
           lv_obj_invalidate_area(celeb_overlay, &u); // drops off-screen parts
           particles[i].box = now;
       }
   }

   // once per refreshed area; must not invalidate (LVGL asserts while rendering)
   static void on_celebration_draw(lv_event_t *e)
   {
       lv_layer_t *layer = lv_event_get_layer(e);
       int32_t left = (int32_t)celeb_flight_ms - celeb_t;
       lv_opa_t opa = (left >= (int32_t)celeb_fade_ms) ? LV_OPA_COVER : (lv_opa_t)(255 * left / celeb_fade_ms);
       for (int i = 0; i < CELEB_PARTICLES && opa > LV_OPA_MIN; i++)
       {
           const Particle *p = &particles[i];
           // _clip_area = area being refreshed (public in 9.2.2 lv_draw.h); a miss costs a draw task
           if (!areas_overlap(&p->box, &layer->_clip_area))
           {
               continue;
           }
           if (p->kind == PK_CONFETTI)
           {
               lv_draw_rect_dsc_t rd;
               lv_draw_rect_dsc_init(&rd); // radius 0, no border or shadow: a plain fill
               rd.bg_color = p->color;
               rd.bg_opa = opa;
               lv_draw_rect(layer, &rd, &p->box);
               continue;
           }
           lv_draw_label_dsc_t ld;
           lv_draw_label_dsc_init(&ld);
           ld.font = &celebrate_22;
           ld.color = p->color;
           ld.opa = opa;
           ld.text = (p->kind == PK_HEART) ? "\xEF\x80\x84" : "\xEF\x80\x85"; // literals outlive the task
           lv_draw_label(layer, &ld, &p->box);
       }
   }

   // tail of celebration_start(day), after the content swap described below
   celeb_overlay = lv_obj_create(lv_layer_top());
   lv_obj_remove_style_all(celeb_overlay); // transparent, no theme border or padding
   lv_obj_set_size(celeb_overlay, 320, 240);
   lv_obj_clear_flag(celeb_overlay, LV_OBJ_FLAG_SCROLLABLE);
   // stays clickable: taps during the burst land here, not in on_screen_clicked
   lv_obj_add_event_cb(celeb_overlay, on_celebration_draw, LV_EVENT_DRAW_MAIN, NULL);
   lv_anim_t a;
   lv_anim_init(&a);
   lv_anim_set_var(&a, celeb_overlay); // deleting the overlay deletes the anim; never NULL
   lv_anim_set_exec_cb(&a, celebration_step);
   lv_anim_set_values(&a, 0, celeb_flight_ms); // linear path: value = ms into the flight
   lv_anim_set_duration(&a, celeb_flight_ms);
   lv_anim_set_delay(&a, celeb_hold_ms);
   lv_anim_set_completed_cb(&a, celebration_completed); // wraps celebration_finish()
   lv_anim_start(&a);
   ```
   - **`celebration_start(day)`, before that tail:**
     - `celebration_finish()` (re-entry only happens from the test hook);
     - `seed_particles(day)`, which reads `hero_bar_*` and falls back to a centre rectangle when the count is 0;
     - `lv_obj_clean(content)`, `shown_hero_day = -1`, `hero_bar_count = 0`;
     - `render_done_message(day)`: "Erledigt!" in `montserrat_de_32`, `0x66BB6A`, over `types_due_on(day)` in `montserrat_de_20`;
     - `update_ack_label()`, the switch to 240 MHz, `celeb_t = 0`, `celeb_start_ms = millis()`;
     - a log line: `celebration start day N: P particles, S source(s), lv mem used X%`.
   - **`celebration_finish()`:** return if `celeb_overlay` is NULL, clear the pointer, then `lv_obj_delete` the overlay.
     - This is safe from the completed callback: the anim has already left the list (lv_anim.c:584-589).
     - From the watchdog it also deletes the anim (lv_obj.c:526).
     - Then restore 80 MHz, print the stats, call `update_display()` and set `last_render_ms = millis()`.
   - **`seed_particles(day)`** follows "Motion" and uses Arduino `random()`. It ends with `particle_box(p, 0, &p->box)` for each particle. Colors:
     - confetti: 2 in 3 get `lv_color_lighten(type_color(bar type), LV_OPA_30)`, so the dark Rest grey and paper blue still read on black; the rest alternate between `0x66BB6A` and white;
     - hearts: `0xEF5350`;
     - stars: `0xFFD54F`.
   - **`celebration_running()`** returns `celeb_overlay != NULL`.
5. **`on_dialog_yes` (main.cpp:481-495).**
   - Copy `dialog_day` and `dialog_undo` into locals before `dialog_close()` resets them.
   - Undo: `ack_remove(day)`, `dialog_close()`, `update_display()`.
   - Otherwise: `ack_set(day)` (written to flash first), `dialog_close()`, `celebration_start(day)`.
6. **`loop()` (main.cpp:711-801).**
   - Move `static uint32_t last_render_ms` from :678 to the globals near :19.
   - Add `&& !celebration_running()` to the fetch condition at :748 and to the periodic branch at :775.
   - Before them, add: `if (celebration_running() && millis() - celeb_start_ms > celeb_watchdog_ms) { Serial.println("celebration watchdog"); celebration_finish(); }`.
7. **Stats and test hooks.**
   - In `setup_ui`, register `lv_display_add_event_cb(lv_display_get_default(), cb, LV_EVENT_FLUSH_START / LV_EVENT_RENDER_READY, NULL)`.
     - While `celeb_overlay && celeb_t > 0`, the flush callback sums `lv_area_get_size(lv_event_get_param(e))` and counts flushes.
     - The render-ready callback counts frames and keeps the per-frame maximum.
     - `celebration_finish` prints them (see Verification).
   - Behind `#ifdef TEST_HOOKS`, `loop()` reads one serial character:

     | Key | Action |
     |---|---|
     | `h` | Append `{today+1, GELB}` and `{today+2, PAPIER}` to `pickups[]` in RAM, `sort_pickups()`, `update_display()`. Nothing is saved: `cache_save` only runs after a fetch. A reboot drops them. |
     | `c` | `celebration_start(shown_hero_day)` without an acknowledgement, so the same hero returns; for tuning. It also sets `last_render_ms = millis() - 59500` so the 60 s tick falls mid-burst. |
     | `x` | Clear all acknowledgements. |
     | `m` | Print `lv_mem_monitor`. |

     Under the same flag, `update_display` prints `render (celebrating=%d)`.
8. **README "Display behavior".**
   - Describe the burst.
   - Explain that each acknowledged day holds until its own 10:00.
   - Explain that undo lifts the latest acknowledgement.

## Resource budget

- **LV_MEM (64 KB, lv_conf.h:50):** the overlay is about 0.16 KB, the two done labels about 0.4 KB, the `lv_anim_t` about 0.1 KB. Short-lived draw tasks (about 70 B each plus a copied descriptor) for at most a few particles per refreshed area add under 1 KB. Clearing the hero frees its 15-20 objects (about 3-4 KB), so peak LV_MEM during a burst is below the hero view's. No layer buffers are used, so the 24 KB `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` budget is untouched. The 38.4 KB draw buffer comes from the internal heap (smartdisplay `lvgl_panel_ili9341_spi.c:36-38`, a single buffer) and is unaffected.
- **Static RAM:** 24 × about 32 B = about 0.8 KB .bss for particles, plus about 0.1 KB for bar geometry and the ack set.
- **Objects:** 3 during a burst (overlay plus 2 labels) instead of 15-20 for the hero.
- **Redraw area per frame:**
  - The SPI floor below is computed. The other per-frame timings are estimates and must be measured.
  - Confetti union at most about 13×29 px, glyph union at most about 33×49 px. Worst case (late, fast fall): 16 × 380 + 8 × 1,600 ≈ 19 k px (25 % of the screen). Mid-flight typically 8-12 k px.
  - SPI floor at 24 MHz and 16 bpp: 13 ms worst, 5-8 ms typical. Add per-area CASET/RASET/RAMWR overhead for at most 24 areas (about 1-4 ms) and the byte swap in the flush callback (`lvgl_panel_ili9341_spi.c:20-26`).
  - The first and last frames are full-screen: 51 ms of SPI plus render, about 60-90 ms each.
- **Expected frame rate:** `LV_DEF_REFR_PERIOD` 33 ms (lv_conf.h:69) and the `delay(10)` loop (main.cpp:800) cap it at about 25-30 fps. At 240 MHz the worst frame is estimated at 15-18 ms, at 80 MHz at 20-27 ms. That is why `celeb_boost_cpu` defaults on. Aim: 20 fps or more.
- **Flash:**
  - New font: about 0.5 KB (306 B bitmap, measured by generating it).
  - Code: about 2-3 KB.
  - No widget enabled in lv_conf.h.
  - The 3 MB `huge_app` slot has room.

## Risks and open questions

- **Frame rate unmeasured.** All timings other than the SPI floor are estimates; nothing has been measured. The knobs are `CELEB_CONFETTI`, the glyph count and `celeb_boost_cpu`.
- **Invalid-area overflow.** If two anim steps land between refreshes, more than 32 invalid areas makes one frame full-screen. Normally both timers run every 33 ms and the refresh timer runs first, so steps and refreshes alternate one to one. If the stats ever show 76,800 px after the hold, add `#define LV_INV_BUF_SIZE 48` to lv_conf.h. It is honoured through the `#ifndef` at lv_display_private.h:28 and costs about 270 B of LV_MEM.
- **`layer->_clip_area` is underscore-prefixed.** It is public in 9.2.2's lv_draw.h:89 and meant for use while draw tasks are created. If an LVGL upgrade breaks it, drop the overlap test: correctness is unchanged, there are just more draw tasks.
- **Glyph trails.** If a glyph draws outside its 28×26 box it leaves trails. Watch for them on the device.
- **CPU clock switch.** The switch inside an LVGL callback may happen while a DMA flush is in flight. APB stays at 80 MHz at both 80 and 240 MHz, as in the existing switch at main.cpp:750. If anything glitches, set `celeb_boost_cpu = false`.
- **Touch mapping.** The double rotation found on hardware (2026-10-16) is removed; re-run the corner check after flashing. If Ja and Nein are swapped, the burst never fires (check the serial log).
- **LittleFS latency.** The `ack_save` write delays the first frame by an unmeasured amount, likely tens of ms.
- **Open: the "Erledigt!" label.** It borders on Plan B's overlay; drop it if the owner wants particles only.
- **Open: skip by tap.** Letting a tap end the burst early would need a guard of about 300 ms against resistive bounce. Also open: `ack_label` shows only the latest acknowledged day.
- **Open: RGB LED and speaker, both not in this plan.** The LED sits on the back of the board. A speaker chirp needs a speaker on the connector; `tone()` uses LEDC channel 0 and the backlight uses the last channel, so they do not clash.
- **Verified against the LVGL v9.2.2 tag source:**
  - `lv_anim_set_completed_cb`, `lv_anim_set_exec_cb` (`void (*)(void *, int32_t)`), `lv_anim_set_duration`, `lv_anim_set_delay`, `lv_anim_set_values`, `lv_anim_set_var`, `lv_anim_start`; early apply is on by default.
  - `lv_event_get_layer`, `lv_draw_rect(_dsc_init)`, `lv_draw_label(_dsc_init)` (`text_local = 0` keeps the pointer).
  - `lv_obj_invalidate_area`, `lv_obj_remove_style_all`, `lv_obj_delete`, `lv_obj_delete_async`.
  - `lv_display_add_event_cb`, `LV_EVENT_FLUSH_START` / `LV_EVENT_RENDER_READY`, `lv_mem_monitor`, `lv_trigo_sin`, `LV_TRIGO_SIN_MAX`, `LV_MIN` / `LV_MAX` / `LV_ABS`, `lv_area_set`, `lv_area_get_size`, `lv_color_lighten`.
- **Not verified, and nothing was compiled (no PlatformIO here):** Arduino-ESP32 `File::read` at EOF, the `parseInt` timeout claim, `setCpuFrequencyMhz` during DMA, and `PLATFORMIO_BUILD_FLAGS`. The generated font was not compiled either; it comes from the same generator as the existing fonts.

## Verification

1. **Build with hooks:** `PLATFORMIO_BUILD_FLAGS=-DTEST_HOOKS pio run -e esp32-2432S028R -t upload && pio device monitor`.
2. **Check the touch mapping first** (README "Touch"): corners log near `0,0` and `319,239`. "Ja" must log `day N acknowledged`.
3. **Fake an adjacent pair:** press `h`. Before that, check in the list that no real Gelber Sack or Altpapier pickup is within two days; otherwise the hero merges. The hero "Für morgen rausstellen: Gelber Sack" appears.
4. **Tap the hero, then "Ja".** Expect:
   - log: `day N acknowledged`, `celebration start day N: 24 particles, 1 source(s), lv mem used X%`;
   - screen: the hero is gone at once, "Erledigt! / Gelber Sack", the burst from the bar area, no trails, no full-screen flicker during flight;
   - after about 1.6 s: the hero "Übermorgen: Altpapier" with "Erledigt: <date>" bottom right;
   - log: `celebration end: F frames in ~1500 ms, max P px / K flushes per frame`.
   - Pass criteria: F ≥ 30 (20 fps or more), P < 76,800, K ≤ 24.
5. **Acknowledge the Altpapier hero.** It moves to the list, and the Gelber Sack hero must not come back. In the list, tap → "Markierung aufheben? Altpapier …" → Ja: the Altpapier hero returns and Gelber Sack stays acknowledged. Acknowledge it again and reboot: the log shows `ack loaded: 2 day(s)`. The injected dates are gone after the reboot, so the real list shows.
6. **Tap during a burst** (use `c` to replay): only `touch x,y` is logged, no dialog, and the burst runs to the end.
7. **Mid-burst periodic tick:** `c` makes the 60 s tick fall mid-burst. The log must show `render (celebrating=0)` only after `celebration end`.
8. **Frame-rate comparison:** set `celeb_boost_cpu = false`, replay with `c`, and compare F. Keep the boost if 80 MHz gives fewer than 30 frames.
9. **Memory:** press `m` before and after a burst, and confirm max used stays below the hero view's level.
10. **Dark room:** cover the light sensor, tap "Ja"; the burst stays lit.
11. **Clean up:** press `x`, rebuild without `TEST_HOOKS`, and reboot to drop the injected dates.
