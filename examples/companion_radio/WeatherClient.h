#pragma once

#ifdef WITH_AUX_WIFI

#include <Arduino.h>

class WeatherClient {
public:
  void begin();
  void loop();
  bool getCached(char* out, size_t out_len);

private:
  bool fetchNow();

  char _cache[80];
  uint32_t _cache_at_ms;
  bool _has_cache;
};

#endif
