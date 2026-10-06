#pragma once
#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "Config.h"

// The four waste types shown on the display. ChristbaumAbholung exists in the
// dataset but is deliberately not fetched.
enum WasteType : uint8_t
{
    BIO = 0,
    REST,
    GELB,
    PAPIER,
    TYPE_COUNT
};
// spelling in the dataset's muellart column (used as an equality filter)
static const char *muellart_filter[TYPE_COUNT] = {"Biomuell", "Restmuell", "Gelber Sack", "Altpapier"};
static const char *type_display_name[TYPE_COUNT] = {"Biomüll", "Restmüll", "Gelber Sack", "Altpapier"};

struct Pickup
{
    int32_t day; // local civil day as days since 1970-01-01
    uint8_t type;
};

// one year of the four types is 117 entries here, 143 with a weekly Restmüll
// schedule; sized for two such years. refresh_year fails loudly instead of
// truncating when a merge would not fit.
#define MAX_PICKUPS 320
static Pickup pickups[MAX_PICKUPS];
static int pickup_count = 0;
static time_t last_fetch_epoch = 0; // last refresh that got the current year

static const time_t min_valid_epoch = 1600000000; // anything earlier means NTP has not run yet

// why the last fetch window ended the way it did; shown while nothing is cached
enum FetchOutcome : uint8_t
{
    FO_NONE,    // no window has run in this boot
    FO_OK,
    FO_NO_FILE, // portal answered, the year's file is not linked yet
    FO_WIFI,    // association failed
    FO_NET,     // HTTP/TLS/JSON failure, cut stream, or no usable resource id
    FO_ADDR     // the datastore has no rows for the configured address
};
static FetchOutcome last_outcome = FO_NONE;

static const char *cache_path = "/pickups.json";

enum YearResult : uint8_t
{
    YR_OK,      // discovery + all four type queries succeeded
    YR_NO_FILE, // portal reachable, but no CSV resource for that year yet
    YR_FAIL     // network error, bad response, or no candidate id works
};


// ---------------------------------------------------------------- date math

// local civil day index; noon keeps DST switches away from the division
static int32_t day_from_ymd(int y, int m, int d)
{
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    time_t e = mktime(&t);
    return (int32_t)(e / 86400);
}

static void tm_from_day(int32_t day, struct tm *out)
{
    time_t e = (time_t)day * 86400 + 43200; // noon UTC = same civil day in CET/CEST
    localtime_r(&e, out);
}

static int year_of_day(int32_t day)
{
    struct tm t;
    tm_from_day(day, &t);
    return t.tm_year + 1900;
}

static int32_t today_day(struct tm *out_tm)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (out_tm)
    {
        *out_tm = t;
    }
    return day_from_ymd(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}


// ---------------------------------------------------------------- WiFi / NTP

static volatile bool time_synced = false; // set by the SNTP callback after sntp_start

static void timeAvailable(struct timeval *t)
{
    time_synced = true;
    Serial.println("Got time adjustment from NTP");
}

static void sntp_start()
{
    time_synced = false;
    sntp_set_time_sync_notification_cb(timeAvailable);
    configTzTime(timeZone, ntpServer1);
}

// The radio is only on for the weekly fetch window (plus the first NTP sync);
// between windows the device is offline by design: wifi_down disables the
// station, which also stops the core's auto-reconnect. While the station stays
// enabled (boot without time) the core keeps reconnecting on its own, so a link
// that failed here can be up at the next call; SNTP is started on that path
// too, otherwise the clock would never be set after a power outage.
static bool wifi_up()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (time(NULL) < min_valid_epoch)
        {
            sntp_start();
        }
        return true;
    }
    Serial.println("WiFi up...");
    WiFi.mode(WIFI_STA);
    // DHCP; for a fixed IP reserve one for this MAC in the router.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++)
    {
        delay(500);
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi connect failed");
        return false;
    }
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    sntp_start(); // every online window resyncs the clock
    return true;
}

// The first NTP packet leaves 0 to 5 s after the connect and a lost one is
// retried after 15 s, so a short fetch window could end before the answer.
// Waits for the sync started by wifi_up, at most max_ms.
static void wait_time_sync(uint32_t max_ms)
{
    uint32_t start = millis();
    while (!time_synced && millis() - start < max_ms)
    {
        delay(100);
    }
    Serial.printf("NTP %s\n", time_synced ? "synced" : "not answered in time");
}

static void wifi_down()
{
    WiFi.disconnect(true); // drops the association, disables the station and powers the radio down
    WiFi.mode(WIFI_OFF);   // already the case after disconnect(true); states the intent
    Serial.println("WiFi off");
}


// ---------------------------------------------------------------- HTTP

static void setup_tls_client(WiFiClientSecure &client)
{
    client.setInsecure();           // the portal has a valid cert; no store on this flash budget
    client.setHandshakeTimeout(15); // seconds; the library default is 120 s per connection
    // HTTPClient::setTimeout reaches WiFiClientSecure's override, which never sets
    // Stream::_timeout; readBytes (and ArduinoJson on top of it) would give up
    // after the 1 s Stream default. The qualified call sets the base member.
    client.Stream::setTimeout(20000);
}

// GET a JSON url and parse it straight off the TLS stream into doc. Buffering
// the whole body as a String ran out of heap next to the open TLS connection
// (~45 KB), so the body never touches a contiguous buffer. HTTP/1.0 keeps the
// stream free of chunked framing.
static bool fetch_json(const String &url, JsonDocument &doc, JsonDocument &filter)
{
    WiFiClientSecure client;
    setup_tls_client(client);
    HTTPClient https;
    https.setConnectTimeout(10000);
    https.setTimeout(20000);
    https.useHTTP10(true);
    if (!https.begin(client, url))
    {
        Serial.println("HTTPS begin failed");
        return false;
    }
    int code = https.GET();
    bool ok = false;
    if (code == 200)
    {
        WiFiClient *body = https.getStreamPtr(); // NULL once the server has closed
        if (body == NULL)
        {
            Serial.println("GET 200 without a body");
        }
        else
        {
            DeserializationError err = deserializeJson(doc, *body, DeserializationOption::Filter(filter));
            if (err)
            {
                Serial.printf("JSON error: %s\n", err.c_str());
            }
            else
            {
                ok = true;
            }
        }
    }
    else
    {
        Serial.printf("GET failed (%d %s): %s\n", code,
                      (code < 0) ? HTTPClient::errorToString(code).c_str() : "", url.c_str());
    }
    https.end();
    return ok;
}

// percent-encodes every byte outside the RFC 3986 unreserved set, so streets
// with umlauts or reserved characters form a valid request line
static String urlencode(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    String out;
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        bool unreserved = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved)
        {
            out += (char)c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// muellart NULL queries the address across all waste types
static String datastore_url(const char *uuid, const char *muellart, int limit)
{
    String u = String("https://") + portalHost + "/api/action/datastore/search.json?resource_id=" + uuid;
    u += "&filters%5Bstrasse%5D=" + urlencode(addrStrasse);
    u += "&filters%5Bhausnummernergaenzung_von%5D=" + urlencode(addrSegmentVon);
    u += "&filters%5Bplz%5D=" + urlencode(addrPlz);
    if (muellart != NULL)
    {
        u += "&filters%5Bmuellart%5D=" + urlencode(muellart);
    }
    u += "&limit=" + String(limit);
    return u;
}


// ---------------------------------------------------------------- discovery

#define UUID_LEN 36
#define MAX_CANDIDATES 4

static bool valid_uuid(const char *u)
{
    for (int i = 0; i < UUID_LEN; i++)
    {
        char c = u[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
            {
                return false;
            }
        }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

// Reads up to want bytes. Returns as soon as some data is in, 0 when the
// server has closed the stream, -1 when nothing arrived for idle_ms.
// Stream::readBytes would instead wait its full timeout for every short final
// chunk, with the radio on at 240 MHz.
static int read_some(WiFiClient *stream, char *dst, int want, uint32_t idle_ms)
{
    int got = 0;
    uint32_t last = millis();
    while (got < want)
    {
        int a = stream->available();
        if (a > 0)
        {
            int r = stream->read((uint8_t *)dst + got, (a < want - got) ? a : want - got);
            if (r > 0)
            {
                got += r;
                last = millis();
                continue;
            }
        }
        if (got > 0)
        {
            break; // hand over what is there; the caller scans any chunk size
        }
        if (!stream->connected())
        {
            return 0;
        }
        if (millis() - last > idle_ms)
        {
            return -1;
        }
        delay(5);
    }
    return got;
}

// Streams the ~74 KB dataset page and collects, in page order, the resource ids
// whose link text is the year's calendar CSV. Returns the candidate count when
// the whole page was seen, -1 on a network error or a cut stream, so a stall is
// retried hourly instead of being read as "file not published".
static int discover_resource_ids(int year, char cand[][UUID_LEN + 1])
{
    char fname[56];
    snprintf(fname, sizeof(fname), "EBK_Abfallplaner_%d_kalendarisch.csv", year);
    const size_t fname_len = strlen(fname);
    const char *res_marker = "/resource/";

    WiFiClientSecure client;
    setup_tls_client(client);
    HTTPClient https;
    https.setConnectTimeout(10000);
    https.setTimeout(20000);
    https.useHTTP10(true); // plain stream, no chunked framing in the scan window
    if (!https.begin(client, String("https://") + portalHost + datasetPath))
    {
        Serial.println("discovery: HTTPS begin failed");
        return -1;
    }
    int code = https.GET();
    if (code != 200)
    {
        Serial.printf("discovery GET failed: %d\n", code);
        https.end();
        return -1;
    }
    WiFiClient *stream = https.getStreamPtr();
    if (stream == NULL)
    {
        Serial.println("discovery: 200 without a body");
        https.end();
        return -1;
    }
    int body_size = https.getSize(); // -1 when unknown; the portal sends no Content-Length
    size_t consumed = 0;
    // overlap must cover the longest pattern ("/resource/" + uuid, or the filename)
    const size_t OVERLAP = 128;
    const size_t CHUNK = 1024;
    static char buf[OVERLAP + CHUNK + 1];
    size_t tail = 0;
    char last_uuid[UUID_LEN + 1] = "";
    int n_cand = 0;
    bool complete = false;
    bool more_hits = false;
    uint32_t start_ms = millis();

    while (millis() - start_ms < 30000)
    {
        int r = read_some(stream, buf + tail, (OVERLAP + CHUNK) - tail, 3000);
        if (r <= 0)
        {
            complete = (r == 0); // 0: server closed after the last byte; -1: stall
            break;
        }
        consumed += r;
        size_t scanned = tail; // bytes re-presented from the previous chunk
        size_t len = tail + r;
        buf[len] = 0;

        // walk both patterns in byte order so each filename hit pairs with the
        // resource id most recently seen before it
        char *pos = buf;
        while (pos < buf + len)
        {
            char *pu = strstr(pos, res_marker);
            char *pf = strstr(pos, fname);
            if (pu == NULL && pf == NULL)
            {
                break;
            }
            if (pu != NULL && (pf == NULL || pu < pf))
            {
                char *u = pu + strlen(res_marker);
                if (u + UUID_LEN > buf + len)
                {
                    break; // split across the chunk boundary; the overlap re-scans it
                }
                if (valid_uuid(u))
                {
                    memcpy(last_uuid, u, UUID_LEN);
                    last_uuid[UUID_LEN] = 0;
                }
                pos = u;
            }
            else
            {
                // a hit that ends inside the overlap was counted in the previous
                // pass; counting it again would pair it with a later id
                if (pf + fname_len > buf + scanned && last_uuid[0] != 0)
                {
                    bool dup = false;
                    for (int i = 0; i < n_cand; i++)
                    {
                        if (strcmp(cand[i], last_uuid) == 0)
                        {
                            dup = true;
                        }
                    }
                    if (!dup && n_cand < MAX_CANDIDATES)
                    {
                        strcpy(cand[n_cand++], last_uuid);
                    }
                    else if (!dup)
                    {
                        more_hits = true;
                    }
                }
                pos = pf + 1;
            }
        }

        if (strstr(buf, "</html>") != NULL)
        {
            complete = true; // end of document; no need to wait for the close
            break;
        }
        if (len > OVERLAP)
        {
            memmove(buf, buf + len - OVERLAP, OVERLAP);
            tail = OVERLAP;
        }
        else
        {
            tail = len;
        }
        if (body_size > 0 && consumed >= (size_t)body_size)
        {
            complete = true;
            break;
        }
    }
    https.end();
    Serial.printf("discovery %d: %u bytes, %d candidate(s)%s, %s\n", year, (unsigned)consumed, n_cand,
                  more_hits ? ", more hits than MAX_CANDIDATES" : "", complete ? "complete" : "INCOMPLETE");
    return complete ? n_cand : -1;
}

enum ProbeResult : uint8_t
{
    PROBE_OK,      // the id answers with rows for the address in the requested year
    PROBE_NO_ROWS, // the id answers, but the address filter matches nothing
    PROBE_BAD      // error object, network failure, or rows of another year
};

// a candidate id is only trusted once the datastore answers a real query on it
static ProbeResult probe_resource(const char *uuid, int year)
{
    JsonDocument filter;
    filter["success"] = true;
    filter["result"]["total"] = true;
    filter["result"]["records"][0]["datum"] = true;
    JsonDocument doc;
    if (!fetch_json(datastore_url(uuid, NULL, 1), doc, filter) || !doc["success"].as<bool>())
    {
        return PROBE_BAD;
    }
    int total = doc["result"]["total"].as<int>();
    const char *ds = doc["result"]["records"][0]["datum"]; // DD.MM.YYYY
    int d, m, y = 0;
    if (ds != NULL && sscanf(ds, "%d.%d.%d", &d, &m, &y) != 3)
    {
        y = 0;
    }
    Serial.printf("probe %s: total %d, first date %s\n", uuid, total, ds ? ds : "-");
    if (total <= 0)
    {
        return PROBE_NO_ROWS;
    }
    // each resource is one calendar year; a row of another year means the page
    // pairing picked the wrong file
    return (y == 0 || y == year) ? PROBE_OK : PROBE_BAD;
}


// ---------------------------------------------------------------- fetching

// Returns the number of dates stored, or -1 when the query failed, returned no
// date of that year, or was cut by the page limit or the staging buffer.
static int fetch_type_year(const char *uuid, uint8_t type, int year, Pickup *out, int out_cap)
{
    const int page = 100;
    JsonDocument filter;
    filter["success"] = true;
    filter["result"]["total"] = true;
    filter["result"]["records"][0]["datum"] = true;
    JsonDocument doc;
    if (!fetch_json(datastore_url(uuid, muellart_filter[type], page), doc, filter))
    {
        return -1;
    }
    if (!doc["success"].as<bool>())
    {
        return -1;
    }
    int total_rows = doc["result"]["total"].as<int>();
    int n = 0;
    for (JsonObject rec : doc["result"]["records"].as<JsonArray>())
    {
        const char *ds = rec["datum"]; // DD.MM.YYYY
        int d, m, y;
        if (ds == NULL || sscanf(ds, "%d.%d.%d", &d, &m, &y) != 3 || y != year)
        {
            continue;
        }
        if (n >= out_cap)
        {
            Serial.printf("%s %d: staging buffer full after %d dates\n", muellart_filter[type], year, n);
            return -1;
        }
        out[n].day = day_from_ymd(y, m, d);
        out[n].type = type;
        n++;
    }
    Serial.printf("%s %d: %d dates of %d rows\n", muellart_filter[type], year, n, total_rows);
    if (total_rows > page)
    {
        return -1; // more rows than one page; a cut year would look complete
    }
    if (n == 0)
    {
        return -1; // a published yearly calendar never has zero dates for a type
    }
    return n;
}

static void sort_pickups()
{
    for (int i = 1; i < pickup_count; i++)
    {
        Pickup key = pickups[i];
        int j = i - 1;
        while (j >= 0 && (pickups[j].day > key.day ||
                          (pickups[j].day == key.day && pickups[j].type > key.type)))
        {
            pickups[j + 1] = pickups[j];
            j--;
        }
        pickups[j + 1] = key;
    }
}


// ---------------------------------------------------------------- cache

static void cache_save()
{
    JsonDocument doc;
    doc["fetched"] = (uint32_t)last_fetch_epoch;
    JsonArray e = doc["e"].to<JsonArray>();
    for (int i = 0; i < pickup_count; i++)
    {
        JsonArray it = e.add<JsonArray>();
        it.add(pickups[i].day);
        it.add(pickups[i].type);
    }
    File f = LittleFS.open(cache_path, "w");
    if (!f)
    {
        Serial.println("cache open for write failed");
        return;
    }
    serializeJson(doc, f);
    f.close();
    Serial.printf("cache saved: %d entries\n", pickup_count);
}

static bool cache_load()
{
    File f = LittleFS.open(cache_path, "r");
    if (!f)
    {
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
        Serial.printf("cache parse error: %s\n", err.c_str());
        return false;
    }
    last_fetch_epoch = doc["fetched"].as<uint32_t>();
    pickup_count = 0;
    int dropped = 0;
    for (JsonArray it : doc["e"].as<JsonArray>())
    {
        if (pickup_count >= MAX_PICKUPS)
        {
            break;
        }
        int32_t day = it[0].as<int32_t>();
        int type = it[1].as<int>();
        if (type < 0 || type >= TYPE_COUNT || day <= 0)
        {
            dropped++; // the type indexes fixed-size arrays in the renderer
            continue;
        }
        pickups[pickup_count].day = day;
        pickups[pickup_count].type = (uint8_t)type;
        pickup_count++;
    }
    sort_pickups();
    Serial.printf("cache loaded: %d entries (%d dropped), fetched %u\n", pickup_count, dropped,
                  (uint32_t)last_fetch_epoch);
    return pickup_count > 0;
}


// ---------------------------------------------------------------- fetch log

// One line per fetch window in LittleFS, printed on boot, so a failed week can
// still be read months later over the serial console. Kept under about 4 KB.
static const char *fetchlog_path = "/fetchlog.txt";

static void fetch_log(const char *msg)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char line[200];
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d %s\n",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, msg);
    Serial.print(line);
    File f = LittleFS.open(fetchlog_path, "a");
    if (!f)
    {
        return;
    }
    f.print(line);
    size_t size = f.size();
    f.close();
    if (size <= 4096)
    {
        return;
    }
    // keep the newest half, starting at a line boundary
    File r = LittleFS.open(fetchlog_path, "r");
    if (!r)
    {
        return;
    }
    r.seek(size - 2048);
    while (r.available() && r.read() != '\n')
    {
    }
    String keep;
    keep.reserve(2100);
    while (r.available())
    {
        keep += (char)r.read();
    }
    r.close();
    File w = LittleFS.open(fetchlog_path, "w");
    if (w)
    {
        w.print(keep);
        w.close();
    }
}

static void fetch_log_dump()
{
    File f = LittleFS.open(fetchlog_path, "r");
    if (!f)
    {
        return;
    }
    Serial.println("--- fetch log ---");
    while (f.available())
    {
        Serial.write(f.read());
    }
    Serial.println("--- end of fetch log ---");
    f.close();
}


// ---------------------------------------------------------------- acknowledgement

// Pickup day the user confirmed on the touch screen as handled (bins are out).
// Its types count as collected until that day's rollover hour. Kept in a
// one-line file so it survives a reboot.
static int32_t acked_day = -1;
static const char *ack_path = "/ack.txt";

static void ack_load()
{
    File f = LittleFS.open(ack_path, "r");
    if (!f)
    {
        return;
    }
    long v = f.parseInt();
    f.close();
    acked_day = (v > 0) ? (int32_t)v : -1;
    Serial.printf("ack loaded: day %ld\n", (long)acked_day);
}

static void ack_set(int32_t day)
{
    acked_day = day;
    File f = LittleFS.open(ack_path, "w");
    if (!f)
    {
        Serial.println("ack open for write failed");
        return;
    }
    f.print(day);
    f.close();
}

static void ack_clear()
{
    acked_day = -1;
    LittleFS.remove(ack_path);
}

// true while the acknowledged day is still ahead under the rollover rule
static bool ack_active(int32_t today, int hour)
{
    return acked_day >= 0 && (acked_day > today || (acked_day == today && hour < rollover_hour));
}


// ---------------------------------------------------------------- refresh

// Replaces all cached pickups of one year with a fresh portal fetch and drops
// past dates of other years, so at most two years stay resident. On any failure
// the cache keeps its previous content.
static YearResult refresh_year(int year, int32_t today)
{
    char cand[MAX_CANDIDATES][UUID_LEN + 1];
    int n_cand = discover_resource_ids(year, cand);
    if (n_cand < 0)
    {
        last_outcome = FO_NET;
        return YR_FAIL;
    }
    if (n_cand == 0)
    {
        Serial.printf("no %d calendar on the dataset page yet\n", year);
        return YR_NO_FILE;
    }
    const char *uuid = NULL;
    bool no_rows = false;
    for (int i = 0; i < n_cand && uuid == NULL; i++)
    {
        ProbeResult pr = probe_resource(cand[i], year);
        if (pr == PROBE_OK)
        {
            uuid = cand[i];
        }
        else if (pr == PROBE_NO_ROWS)
        {
            no_rows = true;
        }
        else
        {
            Serial.printf("candidate %s rejected by datastore\n", cand[i]);
        }
    }
    if (uuid == NULL)
    {
        // an id that answers with zero rows points at the address filters, not at the portal
        last_outcome = no_rows ? FO_ADDR : FO_NET;
        return YR_FAIL;
    }
    Serial.printf("resource id %d: %s\n", year, uuid);

    static Pickup fresh[160]; // one year of the four types is 117 here, 143 with weekly Restmüll
    int total = 0;
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        int n = fetch_type_year(uuid, t, year, fresh + total, 160 - total);
        if (n < 0)
        {
            last_outcome = FO_NET;
            return YR_FAIL; // all four or nothing; a half year would look complete
        }
        total += n;
    }

    int keep = 0;
    for (int i = 0; i < pickup_count; i++)
    {
        if (year_of_day(pickups[i].day) != year && pickups[i].day >= today)
        {
            keep++;
        }
    }
    if (keep + total > MAX_PICKUPS)
    {
        Serial.printf("cache cannot hold %d + %d entries\n", keep, total);
        last_outcome = FO_NET;
        return YR_FAIL; // better than storing a truncated year as complete
    }
    int w = 0;
    for (int i = 0; i < pickup_count; i++)
    {
        // today's pickup stays until the rollover; everything older goes
        if (year_of_day(pickups[i].day) != year && pickups[i].day >= today)
        {
            pickups[w++] = pickups[i];
        }
    }
    for (int i = 0; i < total; i++)
    {
        pickups[w++] = fresh[i];
    }
    pickup_count = w;
    sort_pickups();
    Serial.printf("year %d refreshed: %d dates\n", year, total);
    return YR_OK;
}

// true if anything after (today, hour) per the rollover rule remains cached
static bool have_upcoming(int32_t today, int hour)
{
    for (int i = 0; i < pickup_count; i++)
    {
        if (pickups[i].day > today || (pickups[i].day == today && hour < rollover_hour))
        {
            return true;
        }
    }
    return false;
}

// Fetches the current year (and from December also the next). Returns the delay
// in seconds until the next attempt and appends one line to the fetch log.
static uint32_t refresh_all()
{
    struct tm t;
    int32_t today = today_day(&t);
    int year = t.tm_year + 1900;
    Serial.printf("refresh start, free heap %u\n", (unsigned)ESP.getFreeHeap());

    YearResult cur = refresh_year(year, today);
    // outside December the next year is simply "not there yet"; it must never
    // default to YR_OK or a failed September fetch would save an empty cache
    YearResult nxt = YR_NO_FILE;
    if (t.tm_mon == 11) // December: the next year's file may already exist
    {
        nxt = refresh_year(year + 1, today);
    }

    if (cur == YR_OK)
    {
        last_fetch_epoch = time(NULL);
        last_outcome = FO_OK;
    }
    else if (cur == YR_NO_FILE)
    {
        last_outcome = FO_NO_FILE;
    }
    if (cur == YR_OK || nxt == YR_OK)
    {
        cache_save();
    }

    uint32_t delay_s;
    if (cur == YR_FAIL)
    {
        delay_s = retry_transient_s;
    }
    else if (nxt == YR_FAIL || cur == YR_NO_FILE || !have_upcoming(today, t.tm_hour))
    {
        // the January gap, or the successor file is listed but not yet importable:
        // the current year is fine, so look again daily rather than hourly
        delay_s = retry_gap_s;
    }
    else
    {
        delay_s = refresh_interval_s;
    }

    static const char *yr_name[] = {"ok", "no-file", "FAIL"};
    char msg[160];
    snprintf(msg, sizeof(msg), "%d %s, %d %s, %d dates cached, outcome %d, heap %u, next in %lu s",
             year, yr_name[cur], year + 1, yr_name[nxt], pickup_count, (int)last_outcome,
             (unsigned)ESP.getFreeHeap(), (unsigned long)delay_s);
    fetch_log(msg);
    return delay_s;
}
