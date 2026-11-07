/*
  Jama'ah Beacon -- ESP32 / ST7789, one BOOT button.

  Release BOOT after:
    < 1 second       announce
    1 to < 3 seconds next prayer
    >= 3 seconds     next wait: 2 -> 5 -> 10 -> 2 minutes

  All gestures act ON RELEASE. A long hold never also changes prayer.
  Settings are locked while sending/counting down. STARTING auto-clears
  after 15 seconds; a release can clear it early without announcing.

  Safe first upload: DRY_RUN=true. No Discord requests are made.
  FAST_DRY_RUN makes each selected minute last one second, ONLY in dry run.
  Serial Monitor at 115200: + / - simulate RSVPs during a dry-run countdown.

  Target: classic ESP32 Dev Module; Espressif Arduino core 3.3.12 or newer
  in the 3.x series. ArduinoJson 7; Adafruit GFX; Adafruit ST7735/ST7789;
  Adafruit BusIO. Preserve the existing display wiring below.

  Replace the exposed OLD webhook and bot token in Discord first. Insert
  their NEW values locally. Do not publish a sketch containing credentials.
*/

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <esp_arduino_version.h>
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <time.h>
#include <math.h>

#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#error "Install esp32 by Espressif Systems 3.3.12 or newer (3.x)."
#endif
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "This sketch requires the ESP32 built-in trusted CA certificate bundle."
#endif

// ---------------- Edit these settings locally ----------------
constexpr bool DRY_RUN = false;        // false enables real Discord announcements
constexpr bool FAST_DRY_RUN = false;   // ignored when DRY_RUN is false
const char* WIFI_SSID = "UCSD-GUEST";
const char* WIFI_PASSWORD = ""; // "" for an open network
// Use a plain webhook URL with NO query string or trailing slash.
const char* DISCORD_WEBHOOK =
const char* LOCATION_NAME = "UCSD Contemplation Lounge";

// UCSD / San Diego; ISNA method, Hanafi Asr, Pacific time with DST.
constexpr double LATITUDE = 32.8810;
constexpr double LONGITUDE = -117.2340;
constexpr int CALCULATION_METHOD = 2;
constexpr int ASR_SCHOOL = 0;
const char* TIMEZONE = "PST8PDT,M3.2.0,M11.1.0";
const char* API_TIMEZONE = "America/Los_Angeles";
const char* API_TIMEZONE_ENCODED = "America%2FLos_Angeles";

// ---------------- Existing wiring; no additional button ----------------
constexpr int TFT_CS = 16;
constexpr int TFT_DC = 17;
constexpr int TFT_RST = 21;
constexpr int TFT_SCK = 18;
constexpr int TFT_MOSI = 26;
constexpr int BUTTON_PIN = 0;
Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t PRAYER_HOLD_MS = 1000;
constexpr uint32_t WAIT_HOLD_MS = 3000;
constexpr uint32_t FINISH_HOLD_MS = 15000;
constexpr uint32_t POLL_MS = 5000;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t PRAYER_RETRY_MS = 60000;
constexpr uint8_t WAIT_MINUTES[] = {2, 5, 10};
const char* PRAYERS[] = {"FAJR", "DHUHR", "ASR", "MAGHRIB", "ISHA"};
const char* PRAYER_KEYS[] = {"Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"};
const char* CHECKMARK = "\xE2\x9C\x85";

enum Phase : uint8_t { IDLE, SENDING, FORMING, FINISHED, CHECK_CHAT };
enum Operation : uint8_t { SEND_MESSAGE, ADD_REACTION, READ_RSVPS, READ_TIMES };

// Queue entries contain values, never String objects or borrowed pointers.
struct NetJob {
  Operation op;
  uint32_t generation;
  uint8_t prayer;
  uint8_t minutes;
  int date; // YYYYMMDD
  char message[24];
  char channel[24];
};
struct NetResult {
  Operation op;
  uint32_t generation;
  int code;
  bool ok;
  bool uncertain;
  uint32_t backoffMs;
  int joining;
  int date;
  int times[5];
  int sunrise;
  char message[24];
  char channel[24];
};

// Prototypes with custom types are explicit for Arduino's sketch preprocessor.
void performNetworkJob(const NetJob& job, NetResult& result);
bool submitJob(Operation op);
void networkWorker(void* unused);

QueueHandle_t jobs = nullptr;
QueueHandle_t results = nullptr;
bool workerReady = false;
bool networkBusy = false;
Phase phase = IDLE;
uint32_t generation = 0;
uint8_t prayerIndex = 2; // ASR if the schedule is unavailable
uint8_t waitIndex = 1;  // five-minute default
uint8_t activePrayer = 2;
uint8_t activeMinutes = 5;
bool manualOverride = false;
uint32_t activeDurationMs = 0;
uint32_t startedAt = 0;
uint32_t finishedAt = 0;
uint32_t nextPollAt = 0;
uint32_t nextReactionAt = 0;
uint8_t reactionAttempts = 0;
bool reactionPending = false;
bool pollEnabled = false;
bool rsvpFresh = false;
bool botDisabled = false;
bool webhookDisabled = false;
bool discordPaused = false;
uint32_t discordResumeAt = 0;
char messageId[24] = "";
char channelId[24] = "";
int joiningCount = 0;

bool hasPrayerTimes = false;
int loadedDate = 0;
int prayerMinutes[5] = {-1, -1, -1, -1, -1};
int sunriseMinute = -1;
bool haveFetched = false;
uint32_t lastPrayerFetchAt = 0;
uint32_t lastWiFiRetryAt = 0;
bool wifiConfigured = false;
bool lastWiFiConnected = false;

bool rawButton = HIGH;
bool stableButton = HIGH;
bool buttonArmed = false;
bool pressCaptured = false;
Phase phaseAtPress = IDLE;
uint32_t rawChangedAt = 0;
uint32_t pressedAt = 0;
int previousHoldBand = -1;

char notice[52] = "";
bool noticeActive = false;
uint32_t noticeUntil = 0;
bool displayDirty = true;
uint32_t lastDisplayAt = 0;
int lastDisplayedSecond = -1;
int lastClockMinute = -1;
int lastLocalDate = 0;

bool due(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool localClock(struct tm& local) {
  time_t now = time(nullptr);
  if (now < 1735689600) return false; // wait for NTP, not the 1970 boot clock
  return localtime_r(&now, &local) != nullptr;
}

int dateNumber(const struct tm& local) {
  return (local.tm_year + 1900) * 10000 +
         (local.tm_mon + 1) * 100 + local.tm_mday;
}

void showNotice(const char* text) {
  snprintf(notice, sizeof(notice), "%s", text);
  noticeUntil = millis() + 5000;
  noticeActive = true;
  displayDirty = true;
  Serial.println(text);
}

bool validSnowflake(const char* value) {
  size_t n = strlen(value);
  if (n < 1 || n > 20) return false;
  for (size_t i = 0; i < n; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

bool discordConfigured() {
  String url(DISCORD_WEBHOOK);
  return url.startsWith("https://discord.com/api/webhooks/") &&
         url.indexOf("YOUR_") < 0 && url.indexOf('?') < 0 &&
         url.indexOf('#') < 0 && !url.endsWith("/") &&
         strlen(DISCORD_BOT_TOKEN) > 20 &&
         strstr(DISCORD_BOT_TOKEN, "YOUR_") == nullptr;
}

int parsePrayerTime(const char* value) {
  if (!value || strlen(value) < 5 || value[2] != ':') return -1;
  const int positions[] = {0, 1, 3, 4};
  for (int p : positions) {
    if (value[p] < '0' || value[p] > '9') return -1;
  }
  int hour = (value[0] - '0') * 10 + value[1] - '0';
  int minute = (value[3] - '0') * 10 + value[4] - '0';
  return hour <= 23 && minute <= 59 ? hour * 60 + minute : -1;
}

String announcementText(uint8_t prayer, uint8_t minutes) {
  return String("**") + PRAYERS[prayer] + " congregation forming!**\n\n" +
         "Someone at the **" + LOCATION_NAME + "** plans to pray **" +
         PRAYERS[prayer] + " in " + String(minutes) + " minutes!**\n\n" +
         "React with " + CHECKMARK + " if you're coming to join the jama'ah!";
}

// HTTP and JSON run only in this worker, so slow TLS/requests cannot turn
// a tap into a hold or stop the TFT countdown. The worker never uses the TFT.
void performNetworkJob(const NetJob& job, NetResult& result) {
  result = {};
  result.op = job.op;
  result.generation = job.generation;
  result.date = job.date;
  result.code = -1000; // local preflight failure: no POST attempted
  struct tm local;
  if (WiFi.status() != WL_CONNECTED || !localClock(local)) return;

  String url;
  char date[16];
  snprintf(date, sizeof(date), "%02d-%02d-%04d",
           job.date % 100, (job.date / 100) % 100, job.date / 10000);
  if (job.op == READ_TIMES) {
    url = String("https://api.aladhan.com/v1/timings/") + date +
          "?latitude=" + String(LATITUDE, 6) +
          "&longitude=" + String(LONGITUDE, 6) +
          "&method=" + String(CALCULATION_METHOD) +
          "&school=" + String(ASR_SCHOOL) +
          "&timezonestring=" + API_TIMEZONE_ENCODED;
  } else if (job.op == SEND_MESSAGE) {
    url = String(DISCORD_WEBHOOK) + "?wait=true";
  } else {
    url = String("https://discord.com/api/v10/channels/") + job.channel +
          "/messages/" + job.message;
    if (job.op == ADD_REACTION) url += "/reactions/%E2%9C%85/@me";
  }

  NetworkClientSecure client;
  client.useBuiltinCACertBundle(); // verify CA chain AND server hostname
  client.setHandshakeTimeout(8);   // seconds
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  http.setReuse(false);
  http.useHTTP10(true); // streamed JSON without HTTP chunk framing
  if (!http.begin(client, url)) return;
  const char* headers[] = {
    "Retry-After", "X-RateLimit-Remaining", "X-RateLimit-Reset-After"
  };
  http.collectHeaders(headers, 3);
  http.addHeader("User-Agent", "JamaahBeacon/1.0 (ESP32)");
  if (job.op == ADD_REACTION || job.op == READ_RSVPS) {
    http.addHeader("Authorization", String("Bot ") + DISCORD_BOT_TOKEN);
  }

  if (job.op == SEND_MESSAGE) {
    JsonDocument payload;
    payload["username"] = "Jama'ah Beacon";
    payload["content"] = announcementText(job.prayer, job.minutes);
    payload["allowed_mentions"]["parse"].to<JsonArray>();
    String body;
    serializeJson(payload, body);
    http.addHeader("Content-Type", "application/json");
    result.code = http.POST(body); // intentionally NEVER automatically retried
  } else if (job.op == ADD_REACTION) {
    result.code = http.PUT("");
  } else {
    result.code = http.GET();
  }

  // Filter while reading: do not copy message content, embeds or attachments.
  JsonDocument filter;
  filter["retry_after"] = true;
  if (job.op == SEND_MESSAGE) {
    filter["id"] = true;
    filter["channel_id"] = true;
  } else if (job.op == READ_RSVPS) {
    filter["id"] = true;
    filter["channel_id"] = true;
    filter["reactions"][0]["emoji"]["name"] = true;
    filter["reactions"][0]["count_details"]["normal"] = true;
    filter["reactions"][0]["me"] = true;
  } else if (job.op == READ_TIMES) {
    filter["code"] = true;
    filter["data"]["timings"] = true;
    filter["data"]["date"]["gregorian"]["date"] = true;
    filter["data"]["meta"]["timezone"] = true;
  }
  JsonDocument doc;
  bool parsed = false;
  if (result.code == 200 || result.code == 429) {
    parsed = !deserializeJson(doc, http.getStream(),
                             DeserializationOption::Filter(filter));
  }

  if (job.op != READ_TIMES) {
    double retry = 0;
    if (result.code == 429) {
      double headerRetry = http.header("Retry-After").toDouble();
      double bodyRetry = parsed ? (doc["retry_after"] | 0.0) : 0.0;
      retry = fmax(10.0, fmax(headerRetry, bodyRetry));
    } else if (http.header("X-RateLimit-Remaining") == "0") {
      retry = http.header("X-RateLimit-Reset-After").toDouble();
    }
    if (isfinite(retry) && retry > 0) {
      // At most ~24.8 days, within the wrap-safe signed deadline range.
      result.backoffMs = static_cast<uint32_t>(
        fmin(2147482000.0, ceil(retry * 1000.0) + 500.0));
    }
  }

  if (job.op == SEND_MESSAGE) {
    if (result.code == 200 && parsed) {
      const char* id = doc["id"] | "";
      const char* channel = doc["channel_id"] | "";
      result.ok = validSnowflake(id) && validSnowflake(channel);
      if (result.ok) {
        snprintf(result.message, sizeof(result.message), "%s", id);
        snprintf(result.channel, sizeof(result.channel), "%s", channel);
      }
    }
    // A lost response can mean Discord already posted it. Require inspection
    // and acknowledgement rather than silently retrying and posting twice.
    result.uncertain = !result.ok &&
      (result.code < 0 || result.code >= 500 ||
       (result.code >= 200 && result.code < 300));
  } else if (job.op == ADD_REACTION) {
    result.ok = result.code == 204;
  } else if (job.op == READ_RSVPS && result.code == 200 && parsed) {
    bool valid = strcmp(doc["id"] | "", job.message) == 0 &&
                 strcmp(doc["channel_id"] | "", job.channel) == 0 &&
                 (doc["reactions"].isNull() || doc["reactions"].is<JsonArray>());
    int count = 0;
    for (JsonObject reaction : doc["reactions"].as<JsonArray>()) {
      if (strcmp(reaction["emoji"]["name"] | "", CHECKMARK) != 0) continue;
      if (!reaction["count_details"]["normal"].is<int>() ||
          !reaction["me"].is<bool>()) {
        valid = false;
        break;
      }
      // me refers to THIS bot because this GET authenticates as the bot.
      // Ignore super reactions (one person can also add a normal reaction).
      count = reaction["count_details"]["normal"].as<int>();
      if (reaction["me"].as<bool>() && count > 0) --count;
      if (count < 0) valid = false;
      break;
    }
    result.joining = count;
    result.ok = valid;
  } else if (job.op == READ_TIMES && result.code == 200 && parsed) {
    bool valid = (doc["code"] | 0) == 200 &&
      strcmp(doc["data"]["date"]["gregorian"]["date"] | "", date) == 0 &&
      strcmp(doc["data"]["meta"]["timezone"] | "", API_TIMEZONE) == 0;
    for (int i = 0; i < 5; ++i) {
      result.times[i] = parsePrayerTime(
        doc["data"]["timings"][PRAYER_KEYS[i]] | "");
      if (result.times[i] < 0 ||
          (i > 0 && result.times[i] <= result.times[i - 1])) valid = false;
    }
    result.sunrise = parsePrayerTime(doc["data"]["timings"]["Sunrise"] | "");
    if (result.sunrise <= result.times[0] ||
        result.sunrise >= result.times[1]) valid = false;
    result.ok = valid;
  }
  http.end();
  // Deliberately log only operation/status, never URLs, headers or bodies.
  Serial.printf("Network operation %u: HTTP %d, parsed OK %u\n",
                static_cast<unsigned>(job.op), result.code, result.ok);
}

void networkWorker(void* unused) {
  (void)unused;
  NetJob job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
      NetResult result;
      performNetworkJob(job, result);
      xQueueSend(results, &result, portMAX_DELAY);
    }
  }
}

bool submitJob(Operation op) {
  if (!workerReady || networkBusy) return false;
  NetJob job = {};
  job.op = op;
  job.generation = generation;
  job.prayer = activePrayer;
  job.minutes = activeMinutes;
  if (op == READ_TIMES) {
    struct tm local;
    if (!localClock(local)) return false;
    job.date = dateNumber(local);
  }
  snprintf(job.message, sizeof(job.message), "%s", messageId);
  snprintf(job.channel, sizeof(job.channel), "%s", channelId);
  if (xQueueSend(jobs, &job, 0) != pdTRUE) return false;
  networkBusy = true;
  return true;
}

void beginCountdown() {
  activeDurationMs = static_cast<uint32_t>(activeMinutes) *
    ((DRY_RUN && FAST_DRY_RUN) ? 1000UL : 60000UL);
  startedAt = millis();
  phase = FORMING;
  joiningCount = 0;
  rsvpFresh = DRY_RUN;
  pollEnabled = !DRY_RUN && !botDisabled;
  reactionPending = pollEnabled;
  reactionAttempts = 0;
  nextReactionAt = millis();
  nextPollAt = millis() + POLL_MS;
  lastDisplayedSecond = -1;
  displayDirty = true;
  Serial.printf("Countdown: %s, selected %u min, actual %lu ms%s\n",
    PRAYERS[activePrayer], activeMinutes,
    static_cast<unsigned long>(activeDurationMs), DRY_RUN ? " (DEMO)" : "");
}

void resetAnnouncement() {
  ++generation; // discard an in-flight result from the previous congregation
  phase = IDLE;
  manualOverride = false;
  joiningCount = 0;
  rsvpFresh = false;
  reactionPending = false;
  pollEnabled = false;
  messageId[0] = channelId[0] = '\0';
  noticeActive = false;
  lastDisplayedSecond = -1;
  displayDirty = true;
  // Keep waitIndex: the chosen wait applies to the next congregation too.
  // Keep rate limits and invalid-credential flags across an interface reset.
  Serial.println("READY: automatic prayer selection restored; wait retained.");
}

void announce() {
  if (phase != IDLE) return;
  if (!DRY_RUN) {
    if (!discordConfigured()) { showNotice("Fill in NEW Discord credentials."); return; }
    if (webhookDisabled) { showNotice("Webhook rejected: fix credentials, restart."); return; }
    if (!workerReady) { showNotice("Network worker unavailable: restart."); return; }
    if (WiFi.status() != WL_CONNECTED) { showNotice("No Wi-Fi: no announcement sent."); return; }
    struct tm local;
    if (!localClock(local)) { showNotice("Waiting for NTP clock before HTTPS."); return; }
    if (discordPaused) { showNotice("Discord rate limit: wait, then tap again."); return; }
    if (networkBusy) { showNotice("Network busy: tap again in a moment."); return; }
  }
  ++generation;
  activePrayer = prayerIndex;
  activeMinutes = WAIT_MINUTES[waitIndex];
  noticeActive = false;
  messageId[0] = channelId[0] = '\0';
  if (DRY_RUN) {
    Serial.println("DEMO ONLY -- would send:");
    Serial.println(announcementText(activePrayer, activeMinutes));
    beginCountdown();
  } else {
    phase = SENDING;
    displayDirty = true;
    if (!submitJob(SEND_MESSAGE)) {
      phase = IDLE;
      showNotice("Could not queue: no announcement sent.");
    }
  }
}

void consumeNetworkResult() {
  NetResult result;
  if (!workerReady || xQueueReceive(results, &result, 0) != pdTRUE) return;
  networkBusy = false;
  if (result.op != READ_TIMES && result.backoffMs > 0) {
    discordPaused = true;
    discordResumeAt = millis() + result.backoffMs;
  }
  if (result.op == READ_TIMES) {
    struct tm local;
    if (result.ok && localClock(local) && dateNumber(local) == result.date) {
      for (int i = 0; i < 5; ++i) prayerMinutes[i] = result.times[i];
      sunriseMinute = result.sunrise;
      loadedDate = result.date;
      hasPrayerTimes = true;
      displayDirty = true;
      Serial.println("Today's San Diego prayer schedule loaded.");
    }
    return;
  }

  // Invalid tokens/permissions are latched: do not hammer Discord on failure.
  if (result.code == 401 || result.code == 403 ||
      (result.code == 404 && result.op == SEND_MESSAGE)) {
    if (result.op == SEND_MESSAGE) webhookDisabled = true;
    else {
      botDisabled = true;
      reactionPending = pollEnabled = false;
      rsvpFresh = false;
      showNotice("RSVP stopped: check bot access/message.");
    }
  }
  if (result.generation != generation) return;
  if (result.code == 404 && result.op != SEND_MESSAGE) {
    reactionPending = pollEnabled = false;
    rsvpFresh = false;
    showNotice("Message gone: RSVPs stopped for this countdown.");
  }
  if (result.op == SEND_MESSAGE && phase == SENDING) {
    if (result.ok) {
      snprintf(messageId, sizeof(messageId), "%s", result.message);
      snprintf(channelId, sizeof(channelId), "%s", result.channel);
      beginCountdown();
    } else if (result.uncertain) {
      phase = CHECK_CHAT;
      showNotice("Check Discord; tap to acknowledge, no send.");
    } else {
      phase = IDLE;
      showNotice(result.code == 429 ? "Rate limited: wait before another tap." :
                 "Send failed: no countdown started.");
    }
  } else if (result.op == ADD_REACTION && phase == FORMING) {
    if (result.ok) reactionPending = false;
    else if (result.code != 429 && ++reactionAttempts >= 3) {
      reactionPending = false;
      showNotice("Bot check failed; add a normal check in Discord.");
    }
    nextReactionAt = millis() + POLL_MS;
  } else if (result.op == READ_RSVPS && phase == FORMING) {
    if (result.ok) joiningCount = result.joining;
    rsvpFresh = result.ok && WiFi.status() == WL_CONNECTED;
    nextPollAt = millis() + (result.ok ? POLL_MS : 15000UL);
    displayDirty = true;
  }
  displayDirty = true;
}

void maintainWiFi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != lastWiFiConnected) {
    lastWiFiConnected = connected;
    displayDirty = true;
    Serial.println(connected ? "Wi-Fi connected." : "Wi-Fi disconnected.");
    if (!connected) rsvpFresh = false;
  }
  if (wifiConfigured && !connected &&
      millis() - lastWiFiRetryAt >= WIFI_RETRY_MS) {
    lastWiFiRetryAt = millis();
    WiFi.reconnect();
  }
}

void selectCurrentPrayer(const struct tm& local) {
  if (!hasPrayerTimes || manualOverride || phase != IDLE ||
      rawButton == LOW || stableButton == LOW) return;
  int nowMinute = local.tm_hour * 60 + local.tm_min;
  uint8_t selected;
  if (nowMinute >= prayerMinutes[4] || nowMinute < prayerMinutes[0]) selected = 4;
  else if (nowMinute >= prayerMinutes[3]) selected = 3;
  else if (nowMinute >= prayerMinutes[2]) selected = 2;
  else if (nowMinute >= prayerMinutes[1] || nowMinute >= sunriseMinute) selected = 1;
  else selected = 0;
  if (selected != prayerIndex) {
    prayerIndex = selected;
    displayDirty = true;
    Serial.printf("Auto-selected %s\n", PRAYERS[prayerIndex]);
  }
}

void maintainSchedule() {
  struct tm local;
  if (!localClock(local)) return; // manual selection remains available
  int today = dateNumber(local);
  int clockMinute = local.tm_hour * 60 + local.tm_min;
  if (clockMinute != lastClockMinute || today != lastLocalDate) {
    lastClockMinute = clockMinute;
    lastLocalDate = today;
    displayDirty = true;
  }
  if (hasPrayerTimes && loadedDate != today) {
    hasPrayerTimes = false; // never auto-select using yesterday's timetable
    displayDirty = true;
  }
  selectCurrentPrayer(local);
  // Fetch only while idle, with BOOT released. Countdown/Discord take priority.
  if (!hasPrayerTimes && phase == IDLE && rawButton == HIGH &&
      stableButton == HIGH && !networkBusy && workerReady &&
      WiFi.status() == WL_CONNECTED &&
      (!haveFetched || millis() - lastPrayerFetchAt >= PRAYER_RETRY_MS)) {
    if (submitJob(READ_TIMES)) {
      haveFetched = true;
      lastPrayerFetchAt = millis();
    }
  }
}

void maintainCountdownAndDiscord() {
  uint32_t now = millis();
  if (discordPaused && due(now, discordResumeAt)) {
    discordPaused = false;
    displayDirty = true;
  }
  if (phase == FINISHED) {
    if (now - finishedAt >= FINISH_HOLD_MS) resetAnnouncement();
    return;
  }
  if (phase != FORMING) return;
  uint32_t elapsed = now - startedAt;
  if (elapsed >= activeDurationMs) {
    phase = FINISHED;
    finishedAt = now;
    reactionPending = pollEnabled = false;
    displayDirty = true;
    Serial.println("STARTING! Interface resets in 15 seconds.");
    return;
  }
  int seconds = (activeDurationMs - elapsed + 999UL) / 1000UL;
  if (seconds != lastDisplayedSecond) {
    lastDisplayedSecond = seconds;
    displayDirty = true;
  }
  if (DRY_RUN || botDisabled || discordPaused || networkBusy ||
      WiFi.status() != WL_CONNECTED) return;
  if (reactionPending && due(now, nextReactionAt)) submitJob(ADD_REACTION);
  else if (pollEnabled && due(now, nextPollAt)) submitJob(READ_RSVPS);
}

void handleButton() {
  uint32_t now = millis();
  bool raw = digitalRead(BUTTON_PIN);
  if (raw != rawButton) {
    rawButton = raw;
    rawChangedAt = now;
    if (raw == LOW && stableButton == HIGH && !pressCaptured) phaseAtPress = phase;
  }
  if (now - rawChangedAt < DEBOUNCE_MS || raw == stableButton) return;
  stableButton = raw;
  if (stableButton == LOW) {
    if (buttonArmed) {
      pressedAt = rawChangedAt;
      pressCaptured = true;
      previousHoldBand = -1;
    }
    return;
  }
  buttonArmed = true;
  if (!pressCaptured) return; // ignore BOOT held through startup
  pressCaptured = false;
  uint32_t held = rawChangedAt - pressedAt;
  displayDirty = true;

  // A gesture that started while busy must not announce after an auto-reset.
  if (phaseAtPress == FINISHED && phase == FINISHED) { resetAnnouncement(); return; }
  if (phaseAtPress == CHECK_CHAT && phase == CHECK_CHAT) {
    resetAnnouncement(); // acknowledge only; a second fresh tap is needed to send
    return;
  }
  if (phaseAtPress != IDLE || phase != IDLE) return;
  if (held >= WAIT_HOLD_MS) {
    waitIndex = (waitIndex + 1) % 3;
    char text[48];
    snprintf(text, sizeof(text), "Wait selected: %u minutes", WAIT_MINUTES[waitIndex]);
    showNotice(text);
  } else if (held >= PRAYER_HOLD_MS) {
    prayerIndex = (prayerIndex + 1) % 5;
    manualOverride = true;
    showNotice("Prayer changed (manual until next reset).");
  } else {
    announce();
  }
}

void handleSerialDemo() {
  // No text command can send a real announcement.
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (!DRY_RUN || phase != FORMING) continue;
    if (c == '+') { ++joiningCount; displayDirty = true; }
    else if (c == '-' && joiningCount > 0) { --joiningCount; displayDirty = true; }
  }
}

void printAt(int16_t x, int16_t y, uint8_t size, uint16_t color, const char* text) {
  tft.setCursor(x, y);
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.print(text);
}

void drawDisplay() {
  uint32_t now = millis();
  if (noticeActive && due(now, noticeUntil)) {
    noticeActive = false;
    displayDirty = true;
  }
  if (pressCaptured && stableButton == LOW && phaseAtPress == IDLE) {
    uint32_t held = now - pressedAt;
    int band = held >= WAIT_HOLD_MS ? 2 : (held >= PRAYER_HOLD_MS ? 1 : 0);
    if (band != previousHoldBand) {
      previousHoldBand = band;
      displayDirty = true;
    }
  }
  if (!displayDirty || now - lastDisplayAt < 50) return;
  displayDirty = false;
  lastDisplayAt = now;
  tft.fillRect(245, 8, 75, 28, ST77XX_BLACK);
  tft.fillRect(0, 48, 320, 170, ST77XX_BLACK);

  char text[64];
  struct tm local;
  bool clockOK = localClock(local);
  if (clockOK) snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
  else snprintf(text, sizeof(text), "--:--");
  printAt(250, 14, 2, ST77XX_WHITE, text);

  uint8_t shownPrayer = phase == IDLE || phase == CHECK_CHAT ? prayerIndex : activePrayer;
  printAt(10, 54, 3, ST77XX_CYAN, PRAYERS[shownPrayer]);
  const char* status = "READY";
  uint16_t color = ST77XX_GREEN;
  if (phase == SENDING) { status = "SENDING"; color = ST77XX_YELLOW; }
  else if (phase == FORMING) status = "FORMING";
  else if (phase == FINISHED) status = "STARTING!";
  else if (phase == CHECK_CHAT) { status = "CHECK CHAT"; color = ST77XX_RED; }
  else if (DRY_RUN) status = "DEMO";
  else if (webhookDisabled) { status = "WEBHOOK ERR"; color = ST77XX_RED; }
  else if (!discordConfigured()) { status = "SETUP"; color = ST77XX_YELLOW; }
  else if (WiFi.status() != WL_CONNECTED) { status = "NO WIFI"; color = ST77XX_RED; }
  else if (!clockOK) { status = "NTP..."; color = ST77XX_YELLOW; }
  else if (discordPaused) { status = "RATE LIMIT"; color = ST77XX_YELLOW; }
  printAt(175, 54, 2, color, status);
  printAt(10, 84, 1, ST77XX_WHITE,
          manualOverride ? "Prayer: MANUAL" : (hasPrayerTimes ? "Prayer: AUTO" : "Prayer: NO SCHEDULE"));
  printAt(175, 84, 1, ST77XX_WHITE,
          WiFi.status() == WL_CONNECTED ? "Wi-Fi online" : "Wi-Fi offline");

  uint8_t shownMinutes = phase == IDLE || phase == CHECK_CHAT ? WAIT_MINUTES[waitIndex] : activeMinutes;
  snprintf(text, sizeof(text), "Wait: %u min", shownMinutes);
  printAt(10, 108, 2, ST77XX_WHITE, text);
  if (hasPrayerTimes) {
    bool previousNight = !manualOverride && shownPrayer == 4 && clockOK &&
      local.tm_hour * 60 + local.tm_min < prayerMinutes[0];
    if (previousNight) snprintf(text, sizeof(text), "Adhan: previous night");
    else snprintf(text, sizeof(text), "Adhan today: %02d:%02d",
                  prayerMinutes[shownPrayer] / 60, prayerMinutes[shownPrayer] % 60);
  } else snprintf(text, sizeof(text), "Schedule unavailable");
  printAt(175, 110, 1, ST77XX_CYAN, text);
  if (DRY_RUN && FAST_DRY_RUN) printAt(175, 126, 1, ST77XX_YELLOW, "FAST DEMO: min -> sec");
  printAt(10, 135, 1, ST77XX_WHITE, "Starts in:");
  uint32_t duration = static_cast<uint32_t>(shownMinutes) *
    ((DRY_RUN && FAST_DRY_RUN) ? 1000UL : 60000UL);
  int seconds = duration / 1000UL;
  if (phase == FORMING) {
    uint32_t elapsed = now - startedAt;
    seconds = elapsed < activeDurationMs ? (activeDurationMs - elapsed + 999UL) / 1000UL : 0;
  } else if (phase == FINISHED) seconds = 0;
  snprintf(text, sizeof(text), "%02d:%02d", seconds / 60, seconds % 60);
  printAt(10, 147, 4, ST77XX_YELLOW, text);

  const char* hint = "";
  if (pressCaptured && stableButton == LOW && phaseAtPress == IDLE) {
    hint = previousHoldBand == 2 ? "Release: cycle wait time" :
           (previousHoldBand == 1 ? "Release: cycle prayer; keep holding for wait" :
                                    "Release: announce; keep holding to choose");
  } else if (noticeActive) hint = notice;
  else if (phase == FORMING) hint = "Prayer and wait locked during this countdown";
  else if (phase == FINISHED) hint = "Auto-reset in 15s; release BOOT to clear early";
  else if (phase == CHECK_CHAT) hint = "Check Discord first; BOOT only clears this screen";
  else if (DRY_RUN) hint = "DEMO: nothing posted to Discord";
  printAt(10, 183, 1, ST77XX_YELLOW, hint);
  snprintf(text, sizeof(text), "Joining: %d%s", joiningCount,
    phase == FORMING && !rsvpFresh ? " (unconfirmed)" : "");
  printAt(10, 199, 2, ST77XX_CYAN, text);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  rawButton = stableButton = digitalRead(BUTTON_PIN);
  buttonArmed = stableButton == HIGH;
  rawChangedAt = millis();
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setRotation(1); // 320 x 240 landscape, same as the working sketch
  tft.setTextWrap(false);
  tft.fillScreen(ST77XX_BLACK);
  printAt(10, 10, 3, ST77XX_GREEN, "JAMA'AH");
  tft.drawFastHLine(0, 42, 320, ST77XX_WHITE);
  tft.drawFastHLine(0, 219, 320, ST77XX_WHITE);
  printAt(5, 229, 1, ST77XX_GREEN, "Tap announce | 1-3s prayer | 3s+ wait");

  jobs = xQueueCreate(1, sizeof(NetJob));
  results = xQueueCreate(1, sizeof(NetResult));
  if (jobs && results) {
    workerReady = xTaskCreate(networkWorker, "beacon-net", 16384,
                              nullptr, 1, nullptr) == pdPASS;
  }
  if (!workerReady) showNotice("Network worker unavailable; dry run still works.");

  wifiConfigured = strlen(WIFI_SSID) > 0 && strstr(WIFI_SSID, "YOUR_") == nullptr &&
                   strstr(WIFI_PASSWORD, "YOUR_") == nullptr;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (wifiConfigured) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  lastWiFiRetryAt = millis();
  Serial.println("Jama'ah Beacon: release BOOT <1s announce, 1-3s prayer, 3s+ wait.");
  Serial.println(DRY_RUN ? "DRY RUN: no Discord requests. Serial +/- changes demo RSVPs." :
                           "LIVE: full-minute countdowns; HTTPS certificate verification enabled.");
}

void loop() {
  maintainWiFi();
  consumeNetworkResult();
  maintainCountdownAndDiscord();
  handleButton();
  handleSerialDemo();
  maintainSchedule();
  drawDisplay();
  delay(5);
}
