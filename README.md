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

- From two days before a pickup: hero mode. Header "HEUTE:" (until 10:00),
  "Für morgen rausstellen:" or "Übermorgen:", one colored bar per bin due that
  day, remaining types compact below.
- Otherwise: a list of all four types sorted by next date, with weekday, date
  and countdown.

Bin colors follow the real bins: brown (Bio), grey (Rest), yellow (Gelber
Sack), blue (Altpapier). A pickup day leaves the screen at 10:00. If the last
successful fetch is older than 14 days, a grey "Daten vom DD.MM." line
appears; it turns red after 30 days.

In hero mode a tap anywhere on the screen opens a confirm dialog ("Als
erledigt markieren?", Ja / Nein). Ja marks that pickup day as handled: its
bins count as collected until the day's rollover at 10:00, the display moves
on to the next pickup, and a grey "Erledigt: Mo 05.10." line sits
bottom-right. The mark is stored in LittleFS and survives a reboot. While it
is active and no hero is on screen, a tap offers to lift it again. A dialog
nobody answers closes after 15 seconds; a tap on the dimmed backdrop cancels.

## Power

Between fetch windows the device is offline: after the weekly fetch (the NTP
resync rides along in the same window) the WiFi radio powers down and the CPU
drops to 80 MHz; the window itself runs at 240 MHz for the TLS handshakes.
The WiFi icon is hidden while offline on purpose, grey while connecting,
white while online, so a visible icon always means network activity. The
backlight turns off after about a second of darkness via the board's light
sensor and comes back as soon as the room lights up. A touch keeps the
backlight on for 20 seconds in a dark room, so the dialog is readable with
the lights off.

## Touch

The XPT2046 reports in the panel's native 240x320 frame. esp32-smartdisplay
rotates only the LCD when the display rotation changes, and LVGL 9 does not
rotate pointer input, so `setup_touch()` in main.cpp wraps the input device's
read callback and maps every point into the landscape frame. The mapping is
derived from the MADCTL bits the library sets per rotation and has not been
confirmed on hardware yet. Every press logs the mapped point on the
serial console:

```
touch 12,9
```

In landscape a tap near the top-left corner must log values near 0,0 and the
bottom-right corner values near 319,239. If an axis comes out mirrored, flip
`TOUCH_MIRROR_X` or `TOUCH_MIRROR_Y` in `boards/esp32-2432S028R.json`. The
panel is not calibrated; the dialog buttons are 130x56 pixels and sit in the
middle of the screen, where an uncalibrated resistive panel is accurate
enough. A tap mapped with swapped or inverted axes lands on the backdrop or
the card text, which cancels or does nothing. The one exception is a pure
left-right mirror, which would swap Ja and Nein; the corner check above
catches that before the dialog is trusted.

## Setup

1. Copy `src/Config_example.h` to `src/Config.h`, set WiFi credentials and the
   address filters (street, segment, PLZ as spelled in the dataset).
2. Build and flash: `pio run -e esp32-2432S028R -t upload`.

The German labels need umlauts, which LVGL's built-in fonts lack. The fonts in
`src/fonts/` were generated with:

```
npx lv_font_conv@1.5.3 --font Montserrat-Medium.ttf -r 0x20-0x7E \
  --symbols "äöüÄÖÜß" --size 16 --bpp 4 --no-compress --format lvgl \
  -o src/fonts/montserrat_de_16.c
```

(sizes 16, 20 and 32; the TTF ships with the lvgl package under
`scripts/built_in_font/`).
