#include <Arduino.h>
#include <time.h>
#include <WiFi.h>

#include <esp32_smartdisplay.h>
#include <lvgl.h>

#include "trash_functions.h"

LV_FONT_DECLARE(montserrat_de_16);
LV_FONT_DECLARE(montserrat_de_20);
LV_FONT_DECLARE(montserrat_de_32);
LV_FONT_DECLARE(symbol_check_48); // LV_SYMBOL_OK only, for the done overlay

static lv_obj_t *content; // rebuilt on every render
static lv_obj_t *wifi_label;
static lv_obj_t *stale_label;
static lv_obj_t *ack_label; // "Erledigt: <date>" while a pickup day is acknowledged
static int wifi_icon_state = -1;
static int32_t shown_hero_day = -1; // pickup day of the hero on screen, -1 in every other mode

// touch: confirm dialog on the top layer, built once and shown on demand
static lv_obj_t *dialog;
static lv_obj_t *dialog_title;
static lv_obj_t *dialog_text;
static lv_timer_t *dialog_timer; // closes an unanswered dialog
static int32_t dialog_day = -1;  // pickup day the open dialog is about
static bool dialog_undo = false; // true: the dialog offers to lift an acknowledgement
static lv_indev_t *touch_indev = NULL;
static uint32_t touch_wake_until_ms = 0; // backlight forced on until then after a touch

// done overlay: green "Erledigt" card on the top layer after "Ja", built once.
// STAMP: the circle pops; HOLD: static card; REVEAL: card gone, content rebuilt,
// the veil fades out. Timings live here, not in Config.h, so existing Config.h
// files keep compiling.
enum OverlayState : uint8_t
{
    OV_IDLE,
    OV_STAMP,
    OV_HOLD,
    OV_REVEAL
};
static OverlayState overlay_state = OV_IDLE;
static uint32_t overlay_shown_ms = 0; // millis() of the "Ja" that opened the overlay
static lv_obj_t *overlay;             // full-screen backdrop, absorbs every tap while up
static lv_obj_t *overlay_card;
static lv_obj_t *overlay_circle;
static lv_obj_t *overlay_check;
static lv_obj_t *overlay_types;
static lv_obj_t *overlay_hint;
static lv_timer_t *overlay_timer; // ends the hold
static const uint32_t ov_pop_delay_ms = 80; // lets the full-screen dialog-to-card frame finish first
static const uint32_t ov_pop_ms = 300;
static const uint32_t ov_min_ms = 400; // taps before this are the release bounce of "Ja"
static const uint32_t ov_hold_ms = 1800;
static const uint32_t ov_fade_ms = 320;
static const uint32_t ov_stuck_ms = 10000; // update_display stops deferring after this
static const int32_t ov_circle = 76;       // final circle diameter in px

void update_display();
static void overlay_show(int32_t day);
static void overlay_finish();

static const char *weekday_abbrev[7] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

static lv_color_t type_color(uint8_t t)
{
    switch (t)
    {
    case BIO:
        return lv_color_hex(0x8D6E63); // brown bin
    case REST:
        return lv_color_hex(0x616161); // grey bin; dark enough for white 20 px text
    case GELB:
        return lv_color_hex(0xFDD835); // yellow sack
    default:
        return lv_color_hex(0x1565C0); // blue paper bin; same reason
    }
}


// hidden while intentionally offline, grey while associating, white while online
void update_wifi_icon()
{
    int state;
    if (WiFi.getMode() == WIFI_OFF)
    {
        state = 0;
    }
    else
    {
        state = (WiFi.status() == WL_CONNECTED) ? 2 : 1;
    }
    if (state == wifi_icon_state)
    {
        return;
    }
    wifi_icon_state = state;
    if (state == 0)
    {
        lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(wifi_label,
            (state == 2) ? lv_color_white() : lv_palette_main(LV_PALETTE_GREY), 0);
    }
}

static void format_date(char *buf, size_t n, int32_t day)
{
    struct tm t;
    tm_from_day(day, &t);
    snprintf(buf, n, "%s %02d.%02d.", weekday_abbrev[t.tm_wday], t.tm_mday, t.tm_mon + 1);
}

// transparent full-width row: color chip, type name, date + countdown right
static void make_type_row(int y, int h, uint8_t type, int32_t next_day, int32_t today,
                          const lv_font_t *name_font)
{
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_set_size(row, 312, h);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE); // taps fall through to the screen

    lv_obj_t *chip = lv_obj_create(row);
    lv_obj_set_size(chip, 12, h - 14);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(chip, type_color(type), 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_radius(chip, 3, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_font(name, name_font, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_label_set_text(name, type_display_name[type]);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 24, 0);

    lv_obj_t *when = lv_label_create(row);
    lv_obj_set_style_text_font(when, &montserrat_de_16, 0);
    lv_obj_set_style_text_color(when, lv_color_hex(0xE0E0E0), 0);
    char buf[64];
    if (next_day < 0)
    {
        snprintf(buf, sizeof(buf), "--");
    }
    else
    {
        char date[32];
        format_date(date, sizeof(date), next_day);
        int delta = (int)(next_day - today);
        if (delta == 1)
        {
            snprintf(buf, sizeof(buf), "%s  morgen", date);
        }
        else if (delta == 2)
        {
            snprintf(buf, sizeof(buf), "%s  übermorgen", date);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s  in %d Tg.", date, delta);
        }
    }
    lv_label_set_text(when, buf);
    lv_obj_align(when, LV_ALIGN_RIGHT_MID, -4, 0);
}

static lv_obj_t *make_header(const char *text, lv_color_t color)
{
    lv_obj_t *h = lv_label_create(content);
    lv_obj_set_style_text_font(h, &montserrat_de_20, 0);
    lv_obj_set_style_text_color(h, color, 0);
    lv_label_set_text(h, text);
    lv_obj_set_pos(h, 6, 6);
    return h;
}

static_assert(hero_from_days <= 2, "render_hero's headers cover today, tomorrow and the day after");

// hero mode: every type due on next_day gets a full-width colored bar
static void render_hero(int32_t next_day, int delta, const int32_t next_of_type[TYPE_COUNT],
                        int32_t today)
{
    if (delta == 0)
    {
        make_header("HEUTE:", lv_color_hex(0xEF5350));
    }
    else if (delta == 1)
    {
        make_header("Für morgen rausstellen:", lv_color_hex(0xFFA726));
    }
    else
    {
        make_header("Übermorgen:", lv_color_hex(0xE0E0E0));
    }

    int nb = 0;
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (next_of_type[t] == next_day)
        {
            nb++;
        }
    }
    int bar_h = (nb <= 1) ? 54 : (nb == 2) ? 44 : (nb == 3) ? 34 : 28;
    const lv_font_t *bar_font = (nb <= 2) ? &montserrat_de_32 : &montserrat_de_20;

    int y = 34;
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (next_of_type[t] != next_day)
        {
            continue;
        }
        lv_obj_t *bar = lv_obj_create(content);
        lv_obj_set_size(bar, 312, bar_h);
        lv_obj_set_pos(bar, 4, y);
        lv_obj_set_style_bg_color(bar, type_color(t), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = lv_label_create(bar);
        lv_obj_set_style_text_font(name, bar_font, 0);
        lv_obj_set_style_text_color(name, (t == GELB) ? lv_color_black() : lv_color_white(), 0);
        lv_label_set_text(name, type_display_name[t]);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        y += bar_h + 6;
    }

    char date[32];
    format_date(date, sizeof(date), next_day);
    lv_obj_t *dl = lv_label_create(content);
    lv_obj_set_style_text_font(dl, &montserrat_de_16, 0);
    lv_obj_set_style_text_color(dl, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(dl, date);
    lv_obj_set_pos(dl, 4, y + 2); // right edge at 312, flush with the countdown column
    lv_obj_set_width(dl, 308);
    lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_RIGHT, 0);

    y += 28;
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (next_of_type[t] == next_day)
        {
            continue;
        }
        make_type_row(y, 26, t, next_of_type[t], today, &montserrat_de_16);
        y += 28;
    }
}

// quiet mode: one row per type, sorted by next date, types without a date last
static void render_list(const int32_t next_of_type[TYPE_COUNT], int32_t today)
{
    make_header("Abfuhrtermine", lv_color_hex(0xBDBDBD));

    uint8_t order[TYPE_COUNT];
    int n = 0;
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        order[n++] = t;
    }
    for (int i = 1; i < n; i++)
    {
        uint8_t key = order[i];
        int32_t kd = next_of_type[key];
        int j = i - 1;
        while (j >= 0)
        {
            int32_t jd = next_of_type[order[j]];
            bool after = (kd >= 0 && (jd < 0 || jd > kd));
            if (!after)
            {
                break;
            }
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    int y = 40;
    for (int i = 0; i < n; i++)
    {
        make_type_row(y, 42, order[i], next_of_type[order[i]], today, &montserrat_de_20);
        y += 46;
    }
}

// small is set in lv_font_montserrat_12, which covers ASCII only: no umlauts there
static void render_center_message(const char *big, const char *small)
{
    lv_obj_t *l = lv_label_create(content);
    lv_obj_set_style_text_font(l, &montserrat_de_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, big);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *s = lv_label_create(content);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(s, small);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 16);
}

void update_display()
{
    // The done card refers to the hero underneath: rebuilding now would put the next
    // hero ("Übermorgen: Restmüll") under an "Erledigt" stamp. The reveal re-renders
    // by itself. Bounded, so a stuck overlay can never freeze the calendar.
    if (overlay_state == OV_STAMP || overlay_state == OV_HOLD)
    {
        if (millis() - overlay_shown_ms < ov_stuck_ms)
        {
            Serial.println("render deferred: done overlay up");
            return;
        }
        Serial.println("done overlay stuck, closing it");
        overlay_finish();
    }

    lv_obj_clean(content);
    shown_hero_day = -1;

    time_t now = time(NULL);
    if (now < min_valid_epoch)
    {
        render_center_message("Warte auf Uhrzeit...", "NTP-Sync steht noch aus");
        lv_obj_add_flag(stale_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ack_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    struct tm tmnow;
    int32_t today = today_day(&tmnow);
    if (acked_day >= 0 && !ack_active(today, tmnow.tm_hour))
    {
        ack_clear(); // the acknowledged day rolled over on its own
    }
    // a hero tap can only acknowledge a day within hero_from_days; a later day
    // can only come from a corrupt file and, under the high-water rule in
    // pickup_pending, would hide the whole calendar until it expired
    if (acked_day > today + hero_from_days)
    {
        Serial.printf("ack day %ld lies beyond the hero window, cleared\n", (long)acked_day);
        ack_clear();
    }

    // next date per type under the rollover rule (pickup day counts until 10:00);
    // the acknowledged day and every earlier one count as collected
    int32_t next_of_type[TYPE_COUNT] = {-1, -1, -1, -1};
    for (int i = 0; i < pickup_count; i++) // sorted ascending
    {
        int32_t d = pickups[i].day;
        if (!pickup_pending(d, today, tmnow.tm_hour))
        {
            continue;
        }
        if (next_of_type[pickups[i].type] < 0)
        {
            next_of_type[pickups[i].type] = d;
        }
    }

    int32_t next_day = -1;
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (next_of_type[t] >= 0 && (next_day < 0 || next_of_type[t] < next_day))
        {
            next_day = next_of_type[t];
        }
    }

    if (next_day < 0)
    {
        if (pickup_count == 0 && last_outcome != FO_NO_FILE)
        {
            // nothing fetched yet: name the reason instead of blaming the portal
            const char *why = (last_outcome == FO_WIFI)   ? "WLAN-Verbindung fehlgeschlagen"
                              : (last_outcome == FO_ADDR) ? "Adresse nicht im Datensatz"
                              : (last_outcome == FO_NET)  ? "Portal nicht erreichbar"
                                                          : "Lade Kalender...";
            render_center_message("Noch keine Daten", why);
        }
        else
        {
            // the January gap: old calendar exhausted, successor file not published yet
            int hint_year = tmnow.tm_year + 1900 + ((tmnow.tm_mon == 11) ? 1 : 0);
            char buf[40];
            snprintf(buf, sizeof(buf), "Kalender %d fehlt noch", hint_year);
            char sub[64];
            snprintf(sub, sizeof(sub), "Warte auf %s", portalHost);
            render_center_message(buf, sub);
        }
    }
    else if (next_day - today <= hero_from_days)
    {
        render_hero(next_day, (int)(next_day - today), next_of_type, today);
        shown_hero_day = next_day;
    }
    else
    {
        render_list(next_of_type, today);
    }

    // stale cache warning; invisible while fetches succeed
    if (last_fetch_epoch > 0 && now - last_fetch_epoch > (time_t)stale_warn_age_s)
    {
        struct tm tf;
        localtime_r(&last_fetch_epoch, &tf);
        char buf[40];
        snprintf(buf, sizeof(buf), "Daten vom %02d.%02d.", tf.tm_mday, tf.tm_mon + 1);
        lv_label_set_text(stale_label, buf);
        bool red = now - last_fetch_epoch > (time_t)stale_red_age_s;
        lv_obj_set_style_text_color(stale_label,
            red ? lv_palette_main(LV_PALETTE_RED) : lv_color_hex(0x9E9E9E), 0);
        lv_obj_clear_flag(stale_label, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(stale_label, LV_OBJ_FLAG_HIDDEN);
    }

    // acknowledgement hint; with no hero on screen a tap then offers to lift it
    if (acked_day >= 0)
    {
        char date[32];
        format_date(date, sizeof(date), acked_day);
        char buf[48];
        snprintf(buf, sizeof(buf), "Erledigt: %s", date);
        lv_label_set_text(ack_label, buf);
        lv_obj_clear_flag(ack_label, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(ack_label, LV_OBJ_FLAG_HIDDEN);
    }
}


// ---------------------------------------------------------------- touch

// esp32-smartdisplay rotates only the panel when the display rotation changes;
// the XPT2046 keeps reporting in the native 240x320 portrait frame. LVGL 9.2
// maps pointer input into the rotated frame itself (indev_pointer_proc in
// lv_indev.c, the same formula as the panel's MADCTL rotation), so the
// firmware adds no mapping of its own. An earlier read-callback wrapper applied
// that rotation a second time: on hardware the top-left corner then logged
// 29,205 and the top-right corner made LVGL warn "Y is -48 which is smaller
// than zero". Without it the corners measured 36,33 / 287,31 / 290,210 /
// 32,212 clockwise from top-left (see on_touch_pressed): the uncalibrated
// panel stays about 30 px short of the edges, fine for the centred dialog
// buttons. A press on the very edge can still make LVGL warn "Y is -1": the
// driver mirrors X as x_max - x, one pixel past the edge, and LVGL clamps it.

// every press wakes the backlight and logs the mapped point for a mapping check
static void on_touch_pressed(lv_event_t *e)
{
    touch_wake_until_ms = millis() + touch_backlight_s * 1000;
    lv_point_t p;
    lv_indev_get_point(touch_indev, &p);
    Serial.printf("touch %d,%d\n", (int)p.x, (int)p.y);
}

static void setup_touch()
{
    touch_indev = lv_indev_get_next(NULL); // the one smartdisplay_init registered
    if (touch_indev == NULL)
    {
        Serial.println("no touch input device");
        return;
    }
    lv_indev_add_event_cb(touch_indev, on_touch_pressed, LV_EVENT_PRESSED, NULL);
}


// ---------------------------------------------------------------- confirm dialog

static void dialog_close()
{
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(dialog_timer);
    dialog_day = -1;
}

static void on_dialog_timeout(lv_timer_t *t)
{
    dialog_close();
}

static void on_dialog_no(lv_event_t *e)
{
    dialog_close();
}

static void on_dialog_yes(lv_event_t *e)
{
    // A "Ja" still held when the timeout closes the dialog is clicked on release
    // anyway: LVGL keeps a press-locked object pressed and sends CLICKED without
    // checking HIDDEN. dialog_day is -1 by then; ack_set(-1) would lift the mark.
    if (dialog_day < 0)
    {
        Serial.println("late Ja after dialog timeout ignored");
        return;
    }
    if (dialog_undo)
    {
        struct tm t;
        int32_t today = today_day(&t);
        ack_step_back(today, t.tm_hour);
        Serial.printf("acknowledgement of day %ld lifted, now up to day %ld\n", (long)dialog_day,
                      (long)acked_day);
        dialog_close();
        update_display();
        return;
    }
    int32_t day = dialog_day;
    Serial.printf("day %ld acknowledged\n", (long)day);
    ack_set(day);
    // dialog out and card in within one handler: no frame shows the bright hero
    // between them; the content behind is rebuilt at the reveal
    dialog_close();
    overlay_show(day);
}

// comma-separated display names of the types due on day
static void types_due_on(int32_t day, char *buf, size_t n)
{
    buf[0] = 0;
    for (int i = 0; i < pickup_count; i++)
    {
        if (pickups[i].day != day)
        {
            continue;
        }
        if (buf[0] != 0)
        {
            strlcat(buf, ", ", n);
        }
        strlcat(buf, type_display_name[pickups[i].type], n);
    }
    if (buf[0] == 0)
    {
        strlcpy(buf, "Abfuhr", n); // day no longer in the calendar (undo after a refresh)
    }
}

static void dialog_open(int32_t day, bool undo)
{
    dialog_day = day;
    dialog_undo = undo;
    char types[64];
    types_due_on(day, types, sizeof(types));
    char date[32];
    format_date(date, sizeof(date), day);
    char buf[128];
    if (undo)
    {
        lv_label_set_text(dialog_title, "Markierung aufheben?");
        snprintf(buf, sizeof(buf), "%s am %s wieder anzeigen", types, date);
    }
    else
    {
        lv_label_set_text(dialog_title, "Als erledigt markieren?");
        snprintf(buf, sizeof(buf), "%s am %s", types, date);
    }
    lv_label_set_text(dialog_text, buf);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(dialog_timer);
    lv_timer_resume(dialog_timer);
}

// A tap on the hero offers to mark that pickup day as handled. With no hero on
// screen and an acknowledgement still active it offers to lift the
// acknowledgement. In every other state a tap does nothing.
static void on_screen_clicked(lv_event_t *e)
{
    if (time(NULL) < min_valid_epoch)
    {
        return;
    }
    if (shown_hero_day >= 0)
    {
        dialog_open(shown_hero_day, false);
        return;
    }
    struct tm t;
    int32_t today = today_day(&t);
    if (ack_active(today, t.tm_hour))
    {
        dialog_open(acked_day, true);
    }
}

static void make_dialog_button(lv_obj_t *parent, const char *text, lv_color_t bg,
                               lv_align_t align, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 130, 56);
    lv_obj_align(btn, align, 0, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &montserrat_de_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

// Big targets in the middle of the screen: the resistive panel is uncalibrated
// and a wrongly mapped tap should land on the backdrop (cancel) rather than on
// the other button.
static void setup_dialog()
{
    // full-screen backdrop on the top layer; a tap on it cancels
    dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(dialog, 320, 240);
    lv_obj_set_pos(dialog, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(dialog, 0, 0);
    lv_obj_set_style_radius(dialog, 0, 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dialog, on_dialog_no, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_HIDDEN);

    // the card stays clickable so a tap on it neither cancels nor falls through
    lv_obj_t *card = lv_obj_create(dialog);
    lv_obj_set_size(card, 296, 192);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x263238), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    dialog_title = lv_label_create(card);
    lv_obj_set_style_text_font(dialog_title, &montserrat_de_20, 0);
    lv_obj_set_style_text_color(dialog_title, lv_color_white(), 0);
    lv_obj_align(dialog_title, LV_ALIGN_TOP_LEFT, 0, 0);

    dialog_text = lv_label_create(card);
    lv_obj_set_style_text_font(dialog_text, &montserrat_de_16, 0);
    lv_obj_set_style_text_color(dialog_text, lv_color_hex(0xB0BEC5), 0);
    lv_obj_set_width(dialog_text, 272);
    lv_label_set_long_mode(dialog_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(dialog_text, LV_ALIGN_TOP_LEFT, 0, 30);

    make_dialog_button(card, "Ja", lv_color_hex(0x43A047), LV_ALIGN_BOTTOM_LEFT, on_dialog_yes);
    make_dialog_button(card, "Nein", lv_color_hex(0x546E7A), LV_ALIGN_BOTTOM_RIGHT, on_dialog_no);

    dialog_timer = lv_timer_create(on_dialog_timeout, ack_dialog_timeout_s * 1000, NULL);
    lv_timer_pause(dialog_timer);
}


// ---------------------------------------------------------------- done overlay

// The panel renders into one partial buffer and flushes it before the next chunk,
// so a full-screen frame costs over 50 ms of SPI. Only small areas animate (the
// circle), and nothing here uses opa, transforms or clip_corner: those render
// through ARGB layers that a 296 px card would not fit into in the 64 KB pool.
// bg_opa, text_opa and size need no layer.

#ifdef TEST_HOOKS
static uint32_t overlay_frames = 0; // frames rendered since the overlay was shown

static void on_render_ready(lv_event_t *e)
{
    overlay_frames++;
}
#endif

// "Danach: Restmüll übermorgen" when the view after the reveal is another hero,
// empty otherwise. Call after ack_set, so the acknowledged day no longer counts.
static void next_hero_hint(char *buf, size_t n)
{
    buf[0] = 0;
    struct tm t;
    int32_t today = today_day(&t);
    for (int i = 0; i < pickup_count; i++) // sorted ascending: the first pending day is the next hero
    {
        int32_t d = pickups[i].day;
        if (!pickup_pending(d, today, t.tm_hour))
        {
            continue;
        }
        int delta = (int)(d - today);
        if (delta <= hero_from_days)
        {
            char types[64];
            types_due_on(d, types, sizeof(types));
            const char *when = (delta == 0) ? "heute" : (delta == 1) ? "morgen" : "übermorgen";
            snprintf(buf, n, "Danach: %s %s", types, when);
        }
        return;
    }
}

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
#ifdef TEST_HOOKS
    Serial.printf("overlay idle after %lu ms, %lu frames\n", (unsigned long)(millis() - overlay_shown_ms),
                  (unsigned long)overlay_frames);
#else
    Serial.printf("overlay idle after %lu ms\n", (unsigned long)(millis() - overlay_shown_ms));
#endif
}

static void on_overlay_faded(lv_anim_t *a)
{
    overlay_finish();
}

static void overlay_reveal(const char *why)
{
    if (overlay_state != OV_STAMP && overlay_state != OV_HOLD)
    {
        return; // late timer or tap after a forced close, or already revealing
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

static void on_overlay_timer(lv_timer_t *t)
{
    overlay_reveal("timer");
}

static void on_overlay_clicked(lv_event_t *e)
{
    // absorbs every tap; only one after ov_min_ms (not the bounce of "Ja") skips the hold
    if (millis() - overlay_shown_ms >= ov_min_ms)
    {
        overlay_reveal("tap");
    }
}

static void overlay_show(int32_t day)
{
    char types[64];
    types_due_on(day, types, sizeof(types));
    lv_label_set_text(overlay_types, types);
    char hint[112];
    next_hero_hint(hint, sizeof(hint));
    if (hint[0] != 0)
    {
        lv_label_set_text(overlay_hint, hint);
        lv_obj_clear_flag(overlay_hint, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(overlay_hint, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0); // the last fade left it transparent
    lv_obj_clear_flag(overlay_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    overlay_state = OV_STAMP;
    overlay_shown_ms = millis();
#ifdef TEST_HOOKS
    overlay_frames = 0;
#endif

    // early_apply (the lv_anim_init default) sets the 20 px start size right away,
    // so the first card frame already shows the small circle
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, overlay_circle);
    lv_anim_set_exec_cb(&a, overlay_pop_exec);
    lv_anim_set_values(&a, 20, ov_circle);
    lv_anim_set_delay(&a, ov_pop_delay_ms);
    lv_anim_set_duration(&a, ov_pop_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot); // peaks near 78 px
    lv_anim_set_completed_cb(&a, on_overlay_popped);
    lv_anim_start(&a);

    lv_timer_reset(overlay_timer);
    lv_timer_resume(overlay_timer);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("overlay show: day %ld, hint '%s', lv heap free %u, biggest %u\n", (long)day, hint,
                  (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
}

// Built once on the top layer after the dialog, so it draws above it, and shown on
// demand like the dialog.
static void setup_overlay()
{
    // same black veil as the dialog, so swapping dialog for card does not flash the hero
    overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, 320, 240);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, on_overlay_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    // darker than the "Ja" green: white text reads at about 5:1 instead of 3.3:1.
    // Card, row and circle are not clickable, so every tap reaches the backdrop;
    // not scrollable, so the overshoot never shows a scrollbar.
    overlay_card = lv_obj_create(overlay);
    lv_obj_set_size(overlay_card, 296, LV_SIZE_CONTENT);
    lv_obj_center(overlay_card);
    lv_obj_set_style_bg_color(overlay_card, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_border_width(overlay_card, 0, 0);
    lv_obj_set_style_radius(overlay_card, 10, 0);
    lv_obj_set_style_pad_all(overlay_card, 12, 0);
    lv_obj_set_style_pad_row(overlay_card, 6, 0);
    // a column, because the bins and hint lines wrap to one or two lines
    lv_obj_set_flex_flow(overlay_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(overlay_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(overlay_card);
    lv_obj_set_size(row, 240, 84);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    overlay_circle = lv_obj_create(row);
    lv_obj_set_size(overlay_circle, ov_circle, ov_circle);
    lv_obj_align(overlay_circle, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(overlay_circle, lv_color_white(), 0);
    lv_obj_set_style_border_width(overlay_circle, 0, 0);
    lv_obj_set_style_radius(overlay_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(overlay_circle, 0, 0);
    lv_obj_clear_flag(overlay_circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay_circle, LV_OBJ_FLAG_SCROLLABLE);

    // in the card's green: where the square clip of a growing circle lets the glyph
    // past the round edge, it vanishes into the card
    overlay_check = lv_label_create(overlay_circle);
    lv_obj_set_style_text_font(overlay_check, &symbol_check_48, 0);
    lv_obj_set_style_text_color(overlay_check, lv_color_hex(0x2E7D32), 0);
    lv_label_set_text(overlay_check, LV_SYMBOL_OK);
    lv_obj_center(overlay_check);

    lv_obj_t *title = lv_label_create(row);
    lv_obj_set_style_text_font(title, &montserrat_de_32, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Erledigt");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 102, 0);

    overlay_types = lv_label_create(overlay_card);
    lv_obj_set_style_text_font(overlay_types, &montserrat_de_20, 0);
    lv_obj_set_style_text_color(overlay_types, lv_color_hex(0xE8F5E9), 0);
    lv_obj_set_width(overlay_types, 272);
    lv_label_set_long_mode(overlay_types, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(overlay_types, LV_TEXT_ALIGN_CENTER, 0);

    overlay_hint = lv_label_create(overlay_card);
    lv_obj_set_style_text_font(overlay_hint, &montserrat_de_16, 0);
    lv_obj_set_style_text_color(overlay_hint, lv_color_hex(0xFFE082), 0);
    lv_obj_set_width(overlay_hint, 272);
    lv_label_set_long_mode(overlay_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(overlay_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(overlay_hint, LV_OBJ_FLAG_HIDDEN);

    overlay_timer = lv_timer_create(on_overlay_timer, ov_hold_ms, NULL);
    lv_timer_pause(overlay_timer);

#ifdef TEST_HOOKS
    // counts real frames: the refresh timer pauses itself when nothing is invalid,
    // and RENDER_READY skips the runs that found nothing to draw
    lv_display_add_event_cb(lv_display_get_default(), on_render_ready, LV_EVENT_RENDER_READY, NULL);
#endif
}

void setup_ui()
{
    smartdisplay_lcd_set_backlight(0.8);

    // rotate the 240x320 panel to 320x240 landscape; 270 is the orientation
    // turned 180 degrees from the first builds (which used 90)
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE); // a shaky finger still counts as a tap
    lv_obj_add_event_cb(scr, on_screen_clicked, LV_EVENT_CLICKED, NULL);

    content = lv_obj_create(scr);
    lv_obj_set_size(content, 320, 222);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE); // taps reach the screen

    wifi_label = lv_label_create(scr);
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
    lv_obj_align(wifi_label, LV_ALIGN_TOP_RIGHT, -5, 3);
    lv_obj_set_style_text_color(wifi_label, lv_palette_main(LV_PALETTE_GREY), 0);

    stale_label = lv_label_create(scr);
    lv_obj_set_style_text_font(stale_label, &lv_font_montserrat_12, 0);
    lv_obj_align(stale_label, LV_ALIGN_BOTTOM_LEFT, 6, -3);
    lv_obj_add_flag(stale_label, LV_OBJ_FLAG_HIDDEN);

    ack_label = lv_label_create(scr);
    lv_obj_set_style_text_font(ack_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ack_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(ack_label, LV_ALIGN_BOTTOM_RIGHT, -6, -3);
    lv_obj_add_flag(ack_label, LV_OBJ_FLAG_HIDDEN);

    setup_dialog();
    setup_overlay(); // after the dialog: same layer, drawn above it
    setup_touch();
}

static time_t next_fetch_epoch = 0; // 0 = fetch as soon as time is valid
static uint32_t last_wifi_attempt_ms = 0;
static uint32_t brightness_under_threshold_count = 0;
static uint32_t lv_last_tick = 0;
static uint32_t last_render_ms = 0;
static bool had_valid_time = false;

#ifdef TEST_HOOKS
// Serial single-key commands to exercise the touch flow without waiting for a
// real pickup day (build with -e esp32-2432S028R-test). Injected dates live in
// RAM only: cache_save runs after a successful fetch, which replaces this
// year's rows, and a reboot drops them. The acknowledgement of an injected day
// is persisted like any other and would hide a real pickup on that date, so
// send x before the test ends.
//   h  Gelber Sack tomorrow and Altpapier the day after: a hero, and after
//      "Ja" the done card with "Danach: Altpapier übermorgen", then the next hero
//   s  Gelber Sack tomorrow alone: a hero whose done card has no "Danach" line
//   0  Restmüll today: a "HEUTE:" hero; only before the rollover hour, later
//      the key just says so
//   x  lift the acknowledgement
//   u  re-render (logs "render deferred" while the done card is up)
//   w  start a fetch window now. The fetch drops injected dates of the current
//      year; on 30 and 31 December an injected date can fall into next year and
//      would be saved with the cache, so do not use w after h or s on those days.
static void test_inject(uint8_t type, int32_t day)
{
    for (int i = 0; i < pickup_count; i++)
    {
        if (pickups[i].day == day && pickups[i].type == type)
        {
            return; // pressing a key twice must not list a bin twice
        }
    }
    if (pickup_count >= MAX_PICKUPS)
    {
        Serial.println("test: pickup table full");
        return;
    }
    pickups[pickup_count].day = day;
    pickups[pickup_count].type = type;
    pickup_count++;
    sort_pickups();
}

static void test_hooks()
{
    while (Serial.available() > 0)
    {
        int c = Serial.read();
        if (c != 'h' && c != 's' && c != '0' && c != 'x' && c != 'u' && c != 'w')
        {
            continue; // line endings and unknown keys
        }
        if (time(NULL) < min_valid_epoch)
        {
            Serial.println("test: time not valid yet");
            return;
        }
        struct tm t;
        int32_t today = today_day(&t);
        switch (c)
        {
        case 'h':
            test_inject(GELB, today + 1);
            test_inject(PAPIER, today + 2);
            Serial.println("test: Gelber Sack tomorrow, Altpapier the day after");
            break;
        case 's':
            test_inject(GELB, today + 1);
            Serial.println("test: Gelber Sack tomorrow");
            break;
        case '0':
            if (t.tm_hour >= rollover_hour)
            {
                // the rollover rule hides today's pickup from this hour on
                Serial.printf("test: it is past %02d:00, today's pickup would already be hidden; "
                              "use 0 in the morning\n", rollover_hour);
                continue;
            }
            test_inject(REST, today);
            Serial.println("test: Restmüll today");
            break;
        case 'x':
            ack_clear();
            Serial.println("test: acknowledgement lifted");
            break;
        case 'w':
            next_fetch_epoch = 0; // the loop runs the window right after this call
            Serial.println("test: fetch window now");
            break;
        default:
            break;
        }
        update_display();
        last_render_ms = millis();
    }
}
#endif

void setup()
{
    Serial.begin(115200);
    // The zone must not depend on the network: system time survives soft resets,
    // and a boot without WiFi would otherwise run the calendar on UTC.
    setenv("TZ", timeZone, 1);
    tzset();

    smartdisplay_init();
    setup_ui();

    if (!LittleFS.begin(true)) // format on first use
    {
        Serial.println("LittleFS mount failed");
    }
    else
    {
        cache_load();
        ack_load();
        fetch_log_dump();
    }

    update_display();
    lv_refr_now(NULL); // paint the waiting screen before the blocking connect
    lv_last_tick = millis();
    wifi_up(); // first NTP sync; the loop powers the radio down once time is set
    last_wifi_attempt_ms = millis();
    setCpuFrequencyMhz(80); // plenty for LVGL; the fetch window boosts to 240
}

void loop()
{
    uint32_t now_ms = millis();
    lv_tick_inc(now_ms - lv_last_tick);
    lv_last_tick = now_ms;
    lv_timer_handler();
#ifdef TEST_HOOKS
    test_hooks();
#endif

    time_t now = time(NULL);
    bool time_valid = now > min_valid_epoch;

    // bootstrap: nothing works without NTP time; keep trying to reach it
    if (!time_valid && now_ms - last_wifi_attempt_ms > 60000)
    {
        last_wifi_attempt_ms = now_ms;
        wifi_up(); // starts SNTP, also when the core reconnected in the background
    }

    // schedule from the cache age; a stale or empty cache fetches immediately
    if (time_valid && !had_valid_time)
    {
        had_valid_time = true;
        struct tm t;
        int32_t today = today_day(&t);
        if (last_fetch_epoch > 0 && have_upcoming(today, t.tm_hour))
        {
            next_fetch_epoch = last_fetch_epoch + refresh_interval_s;
        }
        // else nothing lies ahead in the cache (first boot, or a reboot in the
        // January gap): fetch now instead of waiting out the weekly slot
        if (now < next_fetch_epoch)
        {
            wifi_down(); // fresh cache, nothing to fetch: offline until due
        }
        update_display(); // leave the "Warte auf Uhrzeit" state right away
        last_render_ms = now_ms;
    }

    // a fetch window blocks the loop for seconds and would freeze the done card
    // mid-animation; a window that comes due while the card is up waits for
    // the reveal, at most about 2.5 s
    if (time_valid && now >= next_fetch_epoch && overlay_state == OV_IDLE)
    {
        setCpuFrequencyMhz(240); // TLS handshakes are sluggish at 80 MHz
        WiFi.mode(WIFI_STA);     // grey icon while associating
        update_wifi_icon();
        lv_refr_now(NULL);
        uint32_t delay_s;
        if (wifi_up())
        {
            update_wifi_icon(); // white for the online window
            lv_refr_now(NULL);
            delay_s = refresh_all();
            wait_time_sync(8000); // let the weekly NTP answer land before the radio goes off
        }
        else
        {
            delay_s = retry_wifi_s;
            last_outcome = FO_WIFI;
            fetch_log("WiFi association failed");
        }
        wifi_down();
        setCpuFrequencyMhz(80);
        next_fetch_epoch = time(NULL) + delay_s;
        Serial.printf("next fetch in %u s\n", delay_s);
        update_display();
        last_render_ms = millis();
    }
    else if (now_ms - last_render_ms > 60000)
    {
        last_render_ms = now_ms;
        update_display();
    }

    update_wifi_icon();

    // dim the backlight in a dark room (CDS sensor)
    float brightness = smartdisplay_lcd_adaptive_brightness_cds();
    if (brightness < brightness_threshold)
    {
        brightness_under_threshold_count++;
        if (brightness_under_threshold_count > 100000)
        {
            brightness_under_threshold_count = 100000;
        }
    }
    else
    {
        brightness_under_threshold_count = 0;
    }
    bool touch_wake = (int32_t)(touch_wake_until_ms - now_ms) > 0; // wrap-safe
    smartdisplay_lcd_set_backlight((brightness_under_threshold_count > 50 && !touch_wake) ? 0 : 0.8);

    delay(10);
}
