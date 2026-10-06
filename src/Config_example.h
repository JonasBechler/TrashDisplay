// Copy this file to Config.h and fill in the missing values

#include <Arduino.h>

static const char *WIFI_SSID = "Your WiFi SSID";
static const char *WIFI_PASSWORD = "Your WiFi Password";

static const char *ntpServer1 = "de.pool.ntp.org";
static const char *timeZone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Europe/Berlin; the switch times are local time before each change

// Konstanz open-data portal (DKAN, CC0). The dataset page links one CSV resource per
// year; the firmware scans that page for the current year's resource id, so a new
// year needs no manual update. The page lists each filename in more than one section
// and not every linked id answers on the datastore API, so all candidates are probed.
static const char *portalHost = "offenedaten-konstanz.de";
static const char *datasetPath = "/dataset/abfallplaner-m-llabfuhrtermine";

// Address segment exactly as spelled in the dataset columns
// strasse / hausnummernergaenzung_von / plz. All three become equality filters.
static const char *addrStrasse = "Egger Str.";
static const char *addrSegmentVon = "2"; // segment 2-19
static const char *addrPlz = "78464";

// display behavior
static const int rollover_hour = 10; // a pickup day leaves the screen at this hour
static const int hero_from_days = 2; // hero mode from this many days before a pickup

// fetch cadence (age-based, all in seconds)
static const uint32_t refresh_interval_s = 7 * 24 * 3600; // normal re-fetch
static const uint32_t retry_transient_s = 3600;           // after a network/query failure
static const uint32_t retry_gap_s = 24 * 3600;            // calendar exhausted, new year's file not published yet
static const uint32_t retry_wifi_s = 5 * 60;              // WiFi association failed at fetch time
static const uint32_t stale_warn_age_s = 14 * 24 * 3600;  // grey "Daten vom DD.MM." line appears
static const uint32_t stale_red_age_s = 30 * 24 * 3600;   // the line turns red

static const float brightness_threshold = 0.5;

// touch
static const uint32_t ack_dialog_timeout_s = 15; // an unanswered confirm dialog closes after this
static const uint32_t touch_backlight_s = 20;    // a touch keeps the backlight on this long in a dark room
