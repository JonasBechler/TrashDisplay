#include <Arduino.h>
#include <time.h>
#include <WiFi.h>

#include <esp32_smartdisplay.h>
#include <lvgl.h>

#include "trash_functions.h"

LV_FONT_DECLARE(montserrat_de_16);
LV_FONT_DECLARE(montserrat_de_20);
LV_FONT_DECLARE(montserrat_de_32);

static lv_obj_t *content; // rebuilt on every render
static lv_obj_t *wifi_label;
static lv_obj_t *stale_label;
static lv_obj_t *ack_label; // "Erledigt: <date>" while a pickup day is acknowledged
static int wifi_icon_state = -1;
static int32_t shown_hero_day = -1; // pickup day of the hero on screen, -1 in every other mode

static lv_indev_t *touch_indev = NULL;
static uint32_t touch_wake_until_ms = 0; // backlight forced on until then after a touch

// undo: after "Erledigt" the hero stays up in its done state for undo_window_s,
// its button turned into "Rückgängig". Afterwards the mark is final until the
// day's rollover. Lives here, not in Config.h, so existing Config.h files keep
// compiling.
static const uint32_t undo_window_s = 30;
static int32_t undo_day = -1;        // day marked by the last "Erledigt" tap, -1 without an undo window
static int32_t undo_prev_ack = -1;   // acked_day before that tap; undo restores it
static uint32_t undo_start_ms = 0;   // millis() of the tap
static lv_timer_t *undo_timer;       // one tick per second: countdown, then the window closes
static lv_obj_t *undo_count = NULL;  // countdown label inside the hero, NULL while not on screen
static uint32_t undo_lifted_ms = 0;  // millis() of the last "Rückgängig"
static bool celebrate = false;       // the next done render plays the in-place celebration
static const uint32_t bounce_ms = 400; // a tap this soon after a toggle is the release bounce
static const int32_t hero_button_h = 46;

// in-place celebration: the done hero is built right away, undo button included,
// and only small parts animate: a check circle pops in the header, the bars fade
// to their dimmed color. No overlay, nothing between the tap and the undo.
static const uint32_t cel_pop_ms = 320;
static const uint32_t cel_dim_ms = 360;
static const int32_t cel_circle = 26;               // final circle diameter in px
static const int32_t cel_cx = 19, cel_cy = 18;      // circle centre in the header row
static const uint8_t bar_dim_mix = 90;              // done bar: this much of the bin color, rest black

void update_display();
static void on_done_clicked(lv_event_t *e);
static void on_undo_clicked(lv_event_t *e);
static void undo_update_count();

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

#include "celebrations.h" // after type_color

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
        if (delta == 0)
        {
            snprintf(buf, sizeof(buf), "%s  heute", date);
        }
        else if (delta == 1)
        {
            snprintf(buf, sizeof(buf), "%s  morgen", date);
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

// grows the header circle around its centre; the check fades in once it fits
static void cel_pop_exec(void *var, int32_t v)
{
    lv_obj_t *circle = (lv_obj_t *)var;
    lv_obj_set_size(circle, v, v);
    lv_obj_set_pos(circle, cel_cx - v / 2, cel_cy - v / 2);
    int32_t o = (v - 14) * 255 / 10;
    lv_obj_set_style_text_opa(lv_obj_get_child(circle, 0), (lv_opa_t)LV_CLAMP(0, o, 255), 0);
}

// v 0..255: bin color to its dimmed color, name to grey. bg_color and
// text_color only: opa or transforms would render the 312 px bars through layers
static void cel_dim_exec(void *var, int32_t v)
{
    lv_obj_t *bar = (lv_obj_t *)var;
    uint8_t t = (uint8_t)(uintptr_t)lv_obj_get_user_data(bar);
    lv_color_t full = type_color(t);
    lv_color_t dim = lv_color_mix(full, lv_color_black(), bar_dim_mix);
    lv_obj_set_style_bg_color(bar, lv_color_mix(dim, full, (uint8_t)v), 0);
    lv_color_t fg = (t == GELB) ? lv_color_black() : lv_color_white();
    lv_obj_set_style_text_color(lv_obj_get_child(bar, 0), lv_color_mix(lv_color_hex(0xBDBDBD), fg, (uint8_t)v), 0);
}

static void start_anim(void *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms,
                       lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a); // early_apply: the first frame already shows the start value
}

// type indices sorted by next date, types without a date last
static void types_by_date(const int32_t next_of_type[TYPE_COUNT], uint8_t order[TYPE_COUNT])
{
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
}

// symbol in the built-in font (the umlaut fonts carry no symbols), text in montserrat_de_20
static lv_obj_t *make_hero_button(int y, lv_color_t bg, const char *symbol, const char *text,
                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(content);
    lv_obj_set_size(btn, 312, hero_button_h);
    lv_obj_set_pos(btn, 4, y);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_column(btn, 10, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sym = lv_label_create(btn);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sym, lv_color_white(), 0);
    lv_label_set_text(sym, symbol);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &montserrat_de_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    return btn;
}

// Hero mode, only for tomorrow's pickup: one full-width colored bar per bin due on
// day, below them the button, below that the other types compact. done: the day
// was just marked; bars dimmed, the button offers the undo with a countdown.
// The button sits between y 90 and 202, inside the band the uncalibrated panel
// reaches (it stops about 30 px short of each edge, see the touch notes).
static void render_hero(int32_t day, const int32_t next_of_type[TYPE_COUNT], int32_t today, bool done)
{
    bool on_day[TYPE_COUNT] = {false, false, false, false};
    int nb = 0;
    for (int i = 0; i < pickup_count; i++)
    {
        if (pickups[i].day == day && !on_day[pickups[i].type])
        {
            on_day[pickups[i].type] = true;
            nb++;
        }
    }

    bool animate = done && celebrate;
    celebrate = false; // later renders of the window (minute tick) show the end state
    lv_obj_t *header = make_header(done ? "Erledigt" : "Morgen rausstellen",
                                   done ? lv_color_hex(0x66BB6A) : lv_color_hex(0xFFA726));
    if (done)
    {
        // white circle with a green check left of the header text
        lv_obj_set_pos(header, 40, 6);
        lv_obj_t *circle = lv_obj_create(content);
        lv_obj_set_size(circle, cel_circle, cel_circle);
        lv_obj_set_pos(circle, cel_cx - cel_circle / 2, cel_cy - cel_circle / 2);
        lv_obj_set_style_bg_color(circle, lv_color_white(), 0);
        lv_obj_set_style_border_width(circle, 0, 0);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(circle, 0, 0);
        lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *check = lv_label_create(circle);
        lv_obj_set_style_text_font(check, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(check, lv_color_hex(0x2E7D32), 0);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_obj_center(check);
        if (animate)
        {
            start_anim(circle, cel_pop_exec, 4, cel_circle, cel_pop_ms, lv_anim_path_overshoot);
        }
    }

    // date in the header row; right edge at 292 keeps the WiFi icon's corner free
    char date[32];
    format_date(date, sizeof(date), day);
    lv_obj_t *dl = lv_label_create(content);
    lv_obj_set_style_text_font(dl, &montserrat_de_16, 0);
    lv_obj_set_style_text_color(dl, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(dl, date);
    lv_obj_set_width(dl, 120);
    lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(dl, 172, 9);

    // sized so header, bars, button and the remaining rows always fit in 222 px
    int bar_h = (nb <= 1) ? 50 : (nb == 2) ? 38 : (nb == 3) ? 30 : 26;
    const lv_font_t *bar_font = (nb <= 2) ? &montserrat_de_32 : &montserrat_de_20;

    int y = 34;
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (!on_day[t])
        {
            continue;
        }
        lv_obj_t *bar = lv_obj_create(content);
        lv_obj_set_size(bar, 312, bar_h);
        lv_obj_set_pos(bar, 4, y);
        // done: the bin color mixed toward black, plain bg_color, no opa layer
        lv_obj_set_style_bg_color(bar, done ? lv_color_mix(type_color(t), lv_color_black(), bar_dim_mix) : type_color(t), 0);
        lv_obj_set_user_data(bar, (void *)(uintptr_t)t);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = lv_label_create(bar);
        lv_obj_set_style_text_font(name, bar_font, 0);
        lv_color_t fg = (t == GELB) ? lv_color_black() : lv_color_white();
        lv_obj_set_style_text_color(name, done ? lv_color_hex(0xBDBDBD) : fg, 0);
        lv_label_set_text(name, type_display_name[t]);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        if (animate)
        {
            start_anim(bar, cel_dim_exec, 0, 255, cel_dim_ms, lv_anim_path_ease_out);
        }
        y += bar_h + 4;
    }

    if (animate)
    {
        // a random particle celebration over the whole screen; never clickable,
        // so the undo button below keeps working while it runs
        lv_color_t bins[TYPE_COUNT];
        int n = 0;
        for (int t = 0; t < TYPE_COUNT; t++)
        {
            if (on_day[t])
            {
                bins[n++] = type_color(t);
            }
        }
        lv_area_t spawn = {4, 34, 315, y - 5};
        celebration_start(spawn, bins, n);
    }

    y += 2;
    if (done)
    {
        lv_obj_t *btn = make_hero_button(y, lv_color_hex(0x546E7A), LV_SYMBOL_REFRESH, "Rückgängig",
                                         on_undo_clicked);
        undo_count = lv_label_create(btn);
        lv_obj_set_style_text_font(undo_count, &montserrat_de_16, 0);
        lv_obj_set_style_text_color(undo_count, lv_color_hex(0xCFD8DC), 0);
        // fixed width: the countdown redraws only this label, never reflows the row
        lv_obj_set_width(undo_count, 40);
        undo_update_count();
    }
    else
    {
        make_hero_button(y, lv_color_hex(0x43A047), LV_SYMBOL_OK, "Erledigt", on_done_clicked);
    }
    y += hero_button_h + 6;

    uint8_t order[TYPE_COUNT];
    types_by_date(next_of_type, order); // nearest first, as in the list
    for (int i = 0; i < TYPE_COUNT; i++)
    {
        uint8_t t = order[i];
        if (on_day[t])
        {
            continue;
        }
        make_type_row(y, 24, t, next_of_type[t], today, &montserrat_de_16);
        y += 26;
    }
}

// quiet mode: one row per type, sorted by next date, types without a date last
static void render_list(const int32_t next_of_type[TYPE_COUNT], int32_t today)
{
    make_header("Abfuhrtermine", lv_color_hex(0xBDBDBD));

    uint8_t order[TYPE_COUNT];
    types_by_date(next_of_type, order);

    int y = 40;
    for (int i = 0; i < TYPE_COUNT; i++)
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
    lv_obj_clean(content);
    shown_hero_day = -1;
    undo_count = NULL; // deleted with the content

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
    // the hero button can only acknowledge tomorrow; a later day can only come
    // from a corrupt file and, under the high-water rule in pickup_pending, would
    // hide the whole calendar until it expired
    if (acked_day > today + 1)
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

    // the mark of the undo window was lifted elsewhere (rollover, corrupt-file
    // check, test key): nothing left to undo
    if (undo_day >= 0 && acked_day != undo_day)
    {
        undo_day = -1;
        lv_timer_pause(undo_timer);
    }

    if (undo_day >= 0)
    {
        render_hero(undo_day, next_of_type, today, true);
        shown_hero_day = undo_day;
    }
    else if (next_day < 0)
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
    else if (next_day - today == 1)
    {
        render_hero(next_day, next_of_type, today, false);
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

    // acknowledgement hint; the done hero of the undo window says it already
    if (acked_day >= 0 && undo_day < 0)
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
// panel stays about 30 px short of the edges, fine for the full-width hero
// button. A press on the very edge can still make LVGL warn "Y is -1": the
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


// ---------------------------------------------------------------- done and undo

static uint32_t undo_remaining_s()
{
    uint32_t elapsed_s = (millis() - undo_start_ms) / 1000;
    return (elapsed_s >= undo_window_s) ? 0 : undo_window_s - elapsed_s;
}

static void undo_update_count()
{
    if (undo_count == NULL)
    {
        return;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%lu s", (unsigned long)undo_remaining_s());
    lv_label_set_text(undo_count, buf);
}

// Called from a button's own CLICKED handler: rebuilding the content there would
// delete the object whose event is still being dispatched, so the render runs
// from the next lv_timer_handler pass instead.
static void render_async(void *unused)
{
    update_display();
}

static void on_undo_tick(lv_timer_t *t)
{
    if (undo_day < 0)
    {
        lv_timer_pause(undo_timer);
        return;
    }
    if (undo_remaining_s() > 0)
    {
        undo_update_count();
        return;
    }
    Serial.printf("undo window for day %ld closed\n", (long)undo_day);
    undo_day = -1;
    lv_timer_pause(undo_timer);
    update_display(); // the done hero gives way to the list
}

// "Erledigt" on the hero: marks the day at once, no confirm step. The next render
// shows the done hero, with "Rückgängig" in the same spot from the first frame,
// and celebrates in place.
static void on_done_clicked(lv_event_t *e)
{
    int32_t day = shown_hero_day;
    if (day < 0 || undo_day >= 0)
    {
        return; // stale button of an older render
    }
    if (millis() - undo_lifted_ms < bounce_ms)
    {
        return; // bounce of the "Rückgängig" that just brought this button back
    }
    undo_prev_ack = acked_day;
    undo_day = day;
    undo_start_ms = millis();
    lv_timer_reset(undo_timer);
    lv_timer_resume(undo_timer);
    ack_set(day);
    Serial.printf("day %ld acknowledged, undo for %lu s\n", (long)day, (unsigned long)undo_window_s);
    celebrate = true;
    lv_async_call(render_async, NULL);
}

// "Rückgängig" within the window: the mark goes back to what it was before the tap
static void on_undo_clicked(lv_event_t *e)
{
    if (undo_day < 0)
    {
        return; // the window closed while the finger was down
    }
    if (millis() - undo_start_ms < bounce_ms)
    {
        return; // bounce of the "Erledigt" tap; the undo button took its place
    }
    Serial.printf("acknowledgement of day %ld undone after %lu ms\n", (long)undo_day,
                  (unsigned long)(millis() - undo_start_ms));
    if (acked_day == undo_day)
    {
        if (undo_prev_ack >= 0)
        {
            ack_set(undo_prev_ack); // update_display drops it if it rolled over meanwhile
        }
        else
        {
            ack_clear();
        }
    }
    celebration_stop();
    undo_day = -1;
    undo_lifted_ms = millis();
    lv_timer_pause(undo_timer);
    lv_async_call(render_async, NULL);
}

static void setup_undo()
{
    undo_timer = lv_timer_create(on_undo_tick, 1000, NULL);
    lv_timer_pause(undo_timer);
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

    setup_undo();
    celebration_setup(); // last on the top layer: particles draw above everything
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
//   h  Gelber Sack tomorrow and Altpapier the day after: a hero for tomorrow
//      with Altpapier "in 2 Tg." among the rows below it
//   s  Gelber Sack tomorrow alone: a hero; "Erledigt" turns it into the done
//      hero (check pops, bars dim) with "Rückgängig" for 30 s, then the list
//   0  Restmüll today: no hero, a list row "heute"; only before the rollover
//      hour, later the key just says so
//   x  lift the acknowledgement (also ends a running undo window)
//   c  cycle the celebration: random, then each kind in turn (logs the choice)
//   u  re-render
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
        if (c == 'c')
        {
            celebration_force = (celebration_force + 2) % (CEL_KINDS + 1) - 1; // -1, 0 .. CEL_KINDS-1
            Serial.printf("test: celebration %s\n",
                          celebration_force < 0 ? "random" : cel_kinds[celebration_force].name);
            continue;
        }
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

    // a fetch window blocks the loop for seconds and would freeze the celebration
    // and the undo countdown; a window that comes due meanwhile waits for the
    // undo window to close, at most undo_window_s
    if (time_valid && now >= next_fetch_epoch && undo_day < 0)
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
