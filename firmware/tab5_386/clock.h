// The Tab5's clock: its RTC (kept by its battery, in UTC) sets the system's time at the start: file dates on the
// card, screenshots' names (screenshot.h), tiny386's CMOS clock (which stays in UTC: it reads gmtime). The time zone,
// for local dates, is a POSIX TZ string kept in NVS ("tz"; UTC until set). Both are set by the web page from the
// browser's clock and zone whenever it is opened (web.h's /api/clock, clock_set), or by serial "clock".
#pragma once
#include <sys/time.h>
#include <time.h>

static bool clock_valid() { return time(nullptr) >= 1704067200; }   // (2024-01-01: the RTC was set at some time)

static void clock_print() {   // the time, UTC and local
  const time_t t = time(nullptr);
  struct tm u, l;
  gmtime_r(&t, &u);
  localtime_r(&t, &l);
  if (!clock_valid()) {
    Serial.println("clock: not set (serial: clock YYYY-MM-DD HH:MM:SS, in UTC)");
    return;
  }
  Serial.printf("clock: %04d-%02d-%02d %02d:%02d:%02d UTC, %04d-%02d-%02d %02d:%02d:%02d local (TZ %s)\n",
                u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, l.tm_year + 1900,
                l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min, l.tm_sec, getenv("TZ") ? getenv("TZ") : "UTC0");
}

static void clock_tz() {   // the time zone set (after setSystemTimeFromRtc, which sets TZ to GMT0)
  setenv("TZ", g_prefs.getString("tz", "UTC0").c_str(), 1);
  tzset();
}

// The RTC's time into the system's, and the time zone: at the start (before the PC: its CMOS clock reads it)
static void clock_setup() {
  if (M5.Rtc.isEnabled()) M5.Rtc.setSystemTimeFromRtc();
  else Serial.println("clock: no RTC");
  clock_tz();
  clock_print();
}

// serial "clock" (the time), "clock YYYY-MM-DD HH:MM:SS" (the RTC and the system's set, in UTC) or "clock tz <TZ>"
// (the time zone, a POSIX TZ string such as PST8PDT,M3.2.0,M11.1.0 or CET-1CEST,M3.5.0,M10.5.0/3; kept)
static void clock_command(const String& arg) {
  int Y, M, D, h, m, s;
  if (arg.startsWith("tz ")) {
    g_prefs.putString("tz", arg.substring(3));
    clock_tz();
  } else if (arg.length() && sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
    m5::rtc_datetime_t dt;
    dt.date.year = Y, dt.date.month = M, dt.date.date = D;
    dt.time.hours = h, dt.time.minutes = m, dt.time.seconds = s;
    xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
    M5.Rtc.setDateTime(dt);
    M5.Rtc.setSystemTimeFromRtc();
    xSemaphoreGive(g_i2c_lock);
    clock_tz();
  }
  clock_print();
}

// The time (UTC, seconds) and the zone from the web page: the RTC written only when it is more than 2 s off, the
// zone kept only when it changed
static void clock_set(time_t utc, const String& tz) {
  if (utc >= 1704067200 && llabs((long long)(utc - time(nullptr))) > 2) {
    const struct timeval tv = {utc, 0};
    settimeofday(&tv, nullptr);
    struct tm u;
    gmtime_r(&utc, &u);
    xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
    M5.Rtc.setDateTime(&u);
    xSemaphoreGive(g_i2c_lock);
    Serial.println("clock: set from the web page's browser");
  }
  if (tz.length() && tz.length() < 64 && tz != g_prefs.getString("tz", "UTC0")) {
    g_prefs.putString("tz", tz);
    clock_tz();
    Serial.printf("clock: time zone %s, from the web page's browser\n", tz.c_str());
  }
}
