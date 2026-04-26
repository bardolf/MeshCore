#include "WeatherClient.h"

#ifdef WITH_AUX_WIFI

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#ifndef WEATHER_LAT
#define WEATHER_LAT 49.9956
#endif
#ifndef WEATHER_LON
#define WEATHER_LON 14.6531
#endif
#ifndef WEATHER_LABEL
#define WEATHER_LABEL "Ricany"
#endif

static const uint32_t CACHE_TTL_MS = 60UL * 60UL * 1000UL; // 1 hour

static const char *wmoCondition(int code) {
  if (code == 0) return "\xE2\x98\x80 jasno";              // ☀
  if (code == 1) return "\xF0\x9F\x8C\xA4 skoro jasno";    // 🌤
  if (code == 2) return "\xE2\x9B\x85 polojasno";          // ⛅
  if (code == 3) return "\xE2\x98\x81 oblacno";            // ☁
  if (code == 45 || code == 48) return "\xF0\x9F\x8C\xAB mlha";  // 🌫
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
    return "\xF0\x9F\x8C\xA7 dest";                        // 🌧
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "\xE2\x9D\x84 snih";                            // ❄
  if (code >= 95 && code <= 99) return "\xE2\x9B\x88 bourka";  // ⛈
  return "?";
}

void WeatherClient::begin() {
  Serial.println("[WX] init");
  _has_cache = false;
  _cache_at_ms = 0;
  _cache[0] = '\0';
}

void WeatherClient::loop() {
  // Lazy refresh: getCached() handles fetch on demand.
}

bool WeatherClient::getCached(char *out, size_t out_len) {
  uint32_t now = millis();
  bool stale = !_has_cache || (now - _cache_at_ms) > CACHE_TTL_MS;
  if (stale) {
    Serial.printf("[WX] cache miss (have=%d age=%lums) -> fetch\n",
                  _has_cache ? 1 : 0,
                  _has_cache ? (unsigned long)(now - _cache_at_ms) : 0UL);
    if (!fetchNow()) {
      Serial.println("[WX] fetch failed, no cache available");
      return false;
    }
  } else {
    Serial.printf("[WX] cache hit (age=%lums) reply='%s'\n",
                  (unsigned long)(now - _cache_at_ms), _cache);
  }
  strncpy(out, _cache, out_len - 1);
  out[out_len - 1] = '\0';
  return true;
}

bool WeatherClient::fetchNow() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WX] WiFi not ready (status=%d), skipping\n",
                  (int)WiFi.status());
    return false;
  }

  Serial.printf("[WX] net ip=%s gw=%s dns=%s rssi=%d heap=%u\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP().toString().c_str(),
                (int)WiFi.RSSI(),
                (unsigned)ESP.getFreeHeap());

  // Stage 1: DNS only.
  IPAddress server_ip;
  {
    uint32_t t0 = millis();
    int ok = WiFi.hostByName("api.open-meteo.com", server_ip);
    Serial.printf("[WX] DNS ok=%d ip=%s (%lums)\n",
                  ok, server_ip.toString().c_str(),
                  (unsigned long)(millis() - t0));
    if (!ok) return false;
  }

  // Stage 2: plain TCP connect.
  {
    WiFiClient tcp;
    tcp.setTimeout(15);
    uint32_t t0 = millis();
    int ok = tcp.connect(server_ip, 80);
    Serial.printf("[WX] TCP ok=%d (%lums) heap=%u\n",
                  ok, (unsigned long)(millis() - t0),
                  (unsigned)ESP.getFreeHeap());
    tcp.stop();
    if (!ok) return false;
  }

  char url[256];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast"
           "?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code"
           "&daily=temperature_2m_max,temperature_2m_min,weather_code"
           "&timezone=Europe%%2FPrague&forecast_days=1",
           (double)WEATHER_LAT, (double)WEATHER_LON);
  Serial.printf("[WX] GET %s\n", url);

  WiFiClient client;
  client.setTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(15000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    Serial.println("[WX] http.begin failed");
    return false;
  }
  uint32_t t0 = millis();
  int code = http.GET();
  Serial.printf("[WX] http code=%d (%lums) err='%s'\n",
                code, (unsigned long)(millis() - t0),
                http.errorToString(code).c_str());
  if (code != 200) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  Serial.printf("[WX] body (%u bytes): %s\n",
                (unsigned)body.length(), body.c_str());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[WX] parse error: %s\n", err.c_str());
    return false;
  }

  float t_now = doc["current"]["temperature_2m"] | 0.0f;
  int code_now = doc["current"]["weather_code"] | -1;
  float t_max = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  float t_min = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  Serial.printf("[WX] parse OK temp=%.1f max=%.1f min=%.1f code=%d\n",
                (double)t_now, (double)t_max, (double)t_min, code_now);

  // °C is UTF-8 0xC2 0xB0 0x43 (\xC2\xB0 + 'C').
  snprintf(_cache, sizeof(_cache),
           "pocasi %s %s %d\xC2\xB0""C (%d\xC2\xB0""C/%d\xC2\xB0""C)",
           WEATHER_LABEL, wmoCondition(code_now),
           (int)lroundf(t_now), (int)lroundf(t_min), (int)lroundf(t_max));
  _cache_at_ms = millis();
  _has_cache = true;
  Serial.printf("[WX] cache='%s'\n", _cache);
  return true;
}

#endif
