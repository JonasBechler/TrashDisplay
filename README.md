# TrashDisplay

ESP32 display for the Konstanz trash collection calendar. Shows the upcoming
pickup dates for one address segment so the bins go out on time. Built for the
Sunton ESP32-2432S028R ("cheap yellow display", ILI9341 240x320), run in
landscape. Derived from the TibberDisplay project; same board setup, LVGL 9
and PlatformIO toolchain.

## Data source

[Abfallplaner dataset](https://offenedaten-konstanz.de/dataset/abfallplaner-m-llabfuhrtermine)
on the Konstanz open-data portal (DKAN, CC0). The portal publishes one CSV
resource per year. The firmware avoids hardcoding the yearly resource id:

1. Once a week it streams the dataset page and scans for
   `EBK_Abfallplaner_<year>_kalendarisch.csv`, collecting the resource ids
   linked next to that filename. The page lists the filename in more than one
   section and not every id answers on the datastore API, so each candidate is
   probed with a one-row query and the first working id wins.
2. With that id it queries the datastore API once per waste type (Biomuell,
   Restmuell, Gelber Sack, Altpapier), filtered by street, segment and PLZ.
   Each type fits into one 100-row page.
3. Parsed dates are cached in LittleFS, so the display keeps working through
   portal outages and reboots. Each refresh drops past dates of other years,
   so the cache holds at most the current and the next year. A refresh that
   returns no dates for a type, more rows than one page, or a resource id
   whose rows belong to another year counts as failed and leaves the cache
   untouched.

From December the fetch also looks for the next year's file and merges it.
When the old year runs out before the new file is published (in 2026 the file
appeared on January 8), the display shows "Kalender 20XX fehlt noch" and
checks daily. Before the first successful fetch the screen says "Noch keine
Daten" with the reason: WiFi association failed, portal not reachable, or the
address filters match no rows. Every fetch window appends one line to
`/fetchlog.txt` in LittleFS (outcome per year, cached dates, free heap, next
attempt); the file is printed on the serial console at boot.

## Display behavior

Two modes:

- The day before a pickup: hero mode. Header "Morgen rausstellen" with the
  date, one colored bar per bin due tomorrow, a green "Erledigt" button below
  them, remaining types compact below that.
- Otherwise (also on the pickup day itself until 10:00): a list of all four
  types sorted by next date, with weekday, date and countdown ("heute",
  "morgen", "in 3 Tg.").

Bin colors follow the real bins: brown (Bio), grey (Rest), yellow (Gelber
Sack), blue (Altpapier). A pickup day leaves the screen at 10:00. If the last
successful fetch is older than 14 days, a grey "Daten vom DD.MM." line
appears; it turns red after 30 days.

The "Erledigt" button on the hero marks tomorrow's pickup as handled right
away, without a confirm step, and the hero turns into its done state in the
same frame: header "Erledigt", and in place of the button a grey "Rückgängig"
button with a countdown. The celebration happens in place and never blocks
the undo: a white check circle pops open next to the header and the bars fade
to a dimmed color, about 0.35 seconds in all. On top of that one of six
particle celebrations plays over the whole screen for 1.3 to 1.8 seconds,
picked at random and never the same twice in a row: confetti, balloons,
fireworks, sparkles, falling check marks or shooting stars
(`src/celebrations.h`). The particles take no taps, so "Rückgängig" works
while they fly. They are drawn by one transparent object on the top layer and
each step redraws only the small boxes around them, about 10k of the
screen's 77k pixels per frame. For 30 seconds after the tap
"Rückgängig" restores the previous state (a tap within 0.4 seconds of the
first is taken as its bounce and ignored); after that the list takes over and
the mark is final. The bins of the marked day count as collected until the
day's rollover at 10:00, and a grey "Erledigt: Mo 05.10." line sits
bottom-right.

A mark covers its day and every earlier pickup day still ahead, each until its
own rollover. It is stored in LittleFS (one day in `/ack.txt`) and survives a
reboot. A fetch window that comes due during the 30 seconds waits for the
undo window to close.

## Power

Between fetch windows the device is offline: after the weekly fetch (the NTP
resync rides along in the same window) the WiFi radio powers down and the CPU
drops to 80 MHz; the window itself runs at 240 MHz for the TLS handshakes.
The WiFi icon is hidden while offline on purpose, grey while connecting,
white while online, so a visible icon always means network activity. The
backlight turns off after about a second of darkness via the board's light
sensor and comes back as soon as the room lights up. A touch keeps the
backlight on for 20 seconds in a dark room, so the undo countdown is readable
with the lights off.

## Touch

The XPT2046 reports in the panel's native 240x320 frame. esp32-smartdisplay
rotates only the LCD when the display rotation changes, and LVGL 9.2 rotates
pointer input itself (`indev_pointer_proc` in `lv_indev.c`), so the firmware
adds no mapping of its own. An earlier build wrapped the input device's read
callback with the same rotation, which applied it twice: taps landed on the
wrong spot (top-left corner logged as 29,205) and LVGL warned "Y is -48 which
is smaller than zero" for the top-right corner. Every press logs the mapped
point on the serial console:

```
touch 12,9
```

In landscape a tap near the top-left corner must log values near 0,0 and the
bottom-right corner values near 319,239. The panel is not calibrated and
stops about 30 px short of each edge: on the test unit the corners read
36,33 / 287,31 / 290,210 / 32,212 clockwise from top-left. A press on the
very edge can make LVGL log "Y is -1 which is smaller than zero"; the driver
mirrors X as `x_max - x`, one pixel past the edge, and LVGL clamps it, so the
line is harmless. If an axis comes out mirrored, flip
`TOUCH_MIRROR_X` or `TOUCH_MIRROR_Y` in `boards/esp32-2432S028R.json`. The
hero button is full width and 46 px tall, always between y 90 and 202, inside
the band the uncalibrated panel reaches. "Erledigt" and "Rückgängig" share
that spot, so a mirrored axis cannot swap them, and a mistaken "Erledigt" is
undone with a second tap in the same place.

## Setup

1. Copy `src/Config_example.h` to `src/Config.h`, set WiFi credentials and the
   address filters (street, segment, PLZ as spelled in the dataset).
2. Build and flash: `pio run -e esp32-2432S028R -t upload`.

To try the touch flow without waiting for a pickup day, flash the test
environment instead (`pio run -e esp32-2432S028R-test -t upload`) and type
single keys in `pio device monitor`: `h` injects Gelber Sack tomorrow and
Altpapier the day after (a hero with Altpapier among the rows), `s` injects
Gelber Sack tomorrow alone, `0` injects Restmüll today (a "heute" list row;
only before the 10:00 rollover, later the key just says so), `x` lifts the
acknowledgement and ends a running undo window, `c` cycles the celebration
(random, then each of the six in turn) for the next "Erledigt", `u` re-renders,
`w` starts a fetch window now. Injected dates live in RAM
only; the acknowledgement of an injected day is stored like any other, so end
a test with `x`. A fetch drops injected dates of the current year; on 30 and
31 December do not send `w` after `h` or `s`, since a date in the next year
would be saved with the cache.

The German labels need umlauts, which LVGL's built-in fonts lack. The fonts in
`src/fonts/` were generated with:

```
npx lv_font_conv@1.5.3 --font Montserrat-Medium.ttf -r 0x20-0x7E \
  --symbols "äöüÄÖÜß" --size 16 --bpp 4 --no-compress --format lvgl \
  -o src/fonts/montserrat_de_16.c
```

(sizes 16, 20 and 32; the TTF ships with the lvgl package under
`scripts/built_in_font/`). `symbol_check_48`, the check mark of the former
done card, is no longer referenced (the in-place check uses the built-in 16 px
symbol font); it is a one-glyph font made from the FontAwesome file in the
same directory:

```
npx lv_font_conv@1.5.3 --font FontAwesome5-Solid+Brands+Regular.woff \
  -r 0xF00C --size 48 --bpp 4 --no-compress --format lvgl \
  -o src/fonts/symbol_check_48.c
```
