#include <Arduino.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "BH1750FVI.h"
#include <MPU9250_WE.h>

#include <esp_sntp.h>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
extern "C" {
  #include "screens.h"
  #include "ui.h"
}
#include "ArduinoOTA.h"
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include "time.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PNGdec.h>
#ifdef local
#  undef local   // PNGdec/zutil.h defines "local" as "static"; conflicts with our variable names
#endif
// Forward declarations
void initSensors();
void updateBrightness();
void updateOrientation();
void updateFade();
static uint8_t luxToBrightness(float lux);
void setBrightness(uint8_t brightness);
void fadeBrightness(uint8_t target, uint32_t duration_ms);
void addWindData(float speed, float direction, float gust);
void drawWindRose();
void updateWindTimestamp();
void cbSyncTime(struct timeval *tv);
void initSNTP();
void setTimezone();
void applyBootOrientation();
float   circularMean(const float* angles, int count);
// Radar
void initRadarCanvases();
void doRadar();
extern objects_t objects;  // Declared in screens.c
extern "C" void tick_screen_main();
const char* ssid = "mikesnet";
const char* password = "springchicken";

float windavg, windgust, winddir, temp;
int hours, mins, secs;
unsigned long localTimeUnix = 0; 
unsigned long s1LastUpdate = 0;
struct tm timeinfo;
bool isSetNtp = false;

// ==================== MQTT ====================
#define MQTT_HOST    IPAddress(192, 168, 50, 197)
#define MQTT_PORT    1883
const char* mqttUser     = "moeburn";
const char* mqttPassword = "minimi";
const char* mqttClientId = "monolith";

AsyncMqttClient mqttClient;
TimerHandle_t   mqttReconnectTimer;

// ── Thread-safe handoff from AsyncTCP task → loop() task ─────────────────────
// onMqttMessage runs on the AsyncTCP task. LVGL and windHistory are owned by
// loop(). We use a FreeRTOS queue to safely pass messages between tasks without
// needing critical sections or mutexes.
struct MqttPending {
    float   avgwind  = 0;
    float   windgust = 0;
    float   angle    = 0;
    float   temp     = 0;
    bool    hasAvg   = false;
    bool    hasGust  = false;
    bool    hasAngle = false;
    bool    hasTemp  = false;
};

static QueueHandle_t mqttQueue;

void connectToMqtt() {
    Serial.println("Connecting to MQTT...");
    mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
    Serial.println("MQTT connected. Subscribing to topics...");
    mqttClient.subscribe("home/joeywind/avgwind",  0);
    Serial.println("  subscribed: home/joeywind/avgwind");
    mqttClient.subscribe("home/joeywind/windgust", 0);
    Serial.println("  subscribed: home/joeywind/windgust");
    mqttClient.subscribe("home/joeywind/angle",    0);
    Serial.println("  subscribed: home/joeywind/angle");
    mqttClient.subscribe("home/outdoortemps/minimumtemp", 0);
    Serial.println("  subscribed: home/outdoortemps/minimumtemp");
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    Serial.println("MQTT disconnected.");
    if (WiFi.isConnected()) {
        xTimerStart(mqttReconnectTimer, 0);
    }
}

// Called on AsyncTCP task — must NOT touch LVGL or windHistory.
void onMqttMessage(char* topic, char* payload,
                   AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total) {
    char payload_str[32];
    size_t copyLen = (len < sizeof(payload_str) - 1) ? len : sizeof(payload_str) - 1;
    memcpy(payload_str, payload, copyLen);
    payload_str[copyLen] = '\0';
    float val = atof(payload_str);

    MqttPending msg = {};
    if (strcmp(topic, "home/joeywind/avgwind") == 0) {
        msg.avgwind = val;
        msg.hasAvg  = true;
    } else if (strcmp(topic, "home/joeywind/windgust") == 0) {
        msg.windgust = val;
        msg.hasGust  = true;
    } else if (strcmp(topic, "home/joeywind/angle") == 0) {
        msg.angle    = val;
        msg.hasAngle = true;
    } else if (strcmp(topic, "home/outdoortemps/minimumtemp") == 0) {
        msg.temp    = val;
        msg.hasTemp = true;
    }
    // Note: AsyncTCP runs in a task context, so xQueueSend is standard, but 
    // xQueueSendFromISR works safely too if you prefer standardizing it like this
    xQueueSendFromISR(mqttQueue, &msg, NULL);  
}

// ==================== CIRCULAR MEAN ====================
// Calculates the circular mean (mean direction) of angles.
// Unlike regular averaging, this correctly handles angular wraparound.
// E.g., circular mean of 1° and 359° ≈ 0°, not 180°.
float circularMean(const float* angles, int count) {
    if (count <= 0) return 0.0f;
    
    float sin_sum = 0.0f;
    float cos_sum = 0.0f;
    
    for (int i = 0; i < count; i++) {
        // Convert angle to radians
        float rad = angles[i] * PI / 180.0f;
        sin_sum += sin(rad);
        cos_sum += cos(rad);
    }
    
    // Get mean angle in radians, then convert back to degrees
    float mean_rad = atan2(sin_sum, cos_sum);
    float mean_deg = mean_rad * 180.0f / PI;
    
    // Normalize to 0-360 range
    while (mean_deg < 0.0f) mean_deg += 360.0f;
    while (mean_deg >= 360.0f) mean_deg -= 360.0f;
    
    return mean_deg;
}

// Called from loop() — safe to touch LVGL and windHistory here.
void applyMqttPending() {
    MqttPending local;
    while (xQueueReceive(mqttQueue, &local, 0) == pdTRUE) {
        if (local.hasAvg) { 
            windavg = local.avgwind; 
            lv_label_set_text_fmt(objects.label_avg, "%.1fkph", windavg); 
        }
        if (local.hasGust) { 
            windgust = local.windgust; 
            lv_label_set_text_fmt(objects.label_gust, "%.1fkph", windgust); 
        }
        if (local.hasAngle) { 
            winddir = local.angle; 
            addWindData(windavg, winddir, windgust); 
        }
        if (local.hasTemp) { 
            temp = local.temp; 
            lv_label_set_text_fmt(objects.label_temp, "%.1f°C", temp); 
        }
    }
}

// ==================== Sensor I2C Bus (GPIO1=SDA, GPIO40=SCL) ====================
// Touch uses Wire (I2C_NUM_1, SDA=19, SCL=45) — sensors get their own TwoWire instance
TwoWire sensorI2C = TwoWire(0);   // I2C_NUM_0

// ==================== Backlight PWM — dual GPIO system ====================
// GPIO38 = CTRL/EN pin  — high = on, low = off. Acts as coarse control.
//   Range used: 102/255 (40%) to 255/255 (100%). NOT inverted.
// GPIO2  = FB pin (filtered PWM via 0Ω + 80nF to GND)
//   Range used: 0/255 (0%) to 99/255 (39%). INVERTED (0 = brightest).
//
// Unified brightness scale 0–255:
//   0   = dimmest still-visible  (GPIO38=204/255, GPIO2=99/255)
//   255 = full brightness        (GPIO38=255/255, GPIO2=0/255)
//
// Mapping (linear interpolation across two zones):
//   Brightness 0–127   → GPIO38 fixed at 255, GPIO2 sweeps 99→0   (FB dimming)
//   Brightness 128–255 → GPIO2  fixed at 0,   GPIO38 sweeps 102→255 (EN dimming)

#define GFX_BL              GPIO_NUM_38
#define GFX_BL_FB           GPIO_NUM_2
#define BL_PWM_FREQ         100000
#define BL_PWM_RESOLUTION   8

// GPIO38 EN pin range
#define BL_EN_MIN           102   // 40% — minimum EN duty that keeps IC on
#define BL_EN_MAX           255

// GPIO2 FB pin range (inverted — lower duty = brighter)
#define BL_FB_DIM           99    // 39% — maximum FB duty (dimmest)
#define BL_FB_BRIGHT        0     // 0%  — minimum FB duty (brightest)

// BH1750 — ambient light sensor for automatic backlight control
BH1750FVI myLux(0x23, &sensorI2C);
static float    bh1750_lux         = -1.0f;
static uint8_t  current_brightness = 255;

// ── setBrightness: apply a unified 0–255 brightness instantly ────────────────
// Call this instead of ledcWrite directly.
void setBrightness(uint8_t brightness) {
    uint8_t en_duty, fb_duty;
    if (brightness >= 128) {
        // Upper half: FB at full brightness, sweep EN from min to max
        float t  = (brightness - 128) / 127.0f;
        en_duty  = (uint8_t)(BL_EN_MIN + t * (BL_EN_MAX - BL_EN_MIN) + 0.5f);
        fb_duty  = BL_FB_BRIGHT;
    } else {
        // Lower half: EN at max, sweep FB from bright to dim (inverted)
        float t  = brightness / 127.0f;
        en_duty  = BL_EN_MAX;
        fb_duty  = (uint8_t)(BL_FB_DIM - t * (BL_FB_DIM - BL_FB_BRIGHT) + 0.5f);
    }
    ledcWrite(GFX_BL,    en_duty);
    ledcWrite(GFX_BL_FB, fb_duty);
}

// ── fadeBrightness: fade from current_brightness to target over ms ───────────
// Non-blocking via static state machine — call from loop() each iteration.
// To trigger a fade, call fadeBrightness(target, ms) once.
// The fade will complete on its own across subsequent loop() calls.
static uint8_t  fade_target    = 255;
static uint8_t  fade_start     = 255;
static uint32_t fade_start_ms  = 0;
static uint32_t fade_duration  = 0;
static bool     fade_active    = false;

void fadeBrightness(uint8_t target, uint32_t duration_ms) {
    if (duration_ms == 0) {
        // Instant
        setBrightness(target);
        current_brightness = target;
        fade_active = false;
        return;
    }
    fade_start    = current_brightness;
    fade_target   = target;
    fade_start_ms = millis();
    fade_duration = duration_ms;
    fade_active   = true;
}

// Call this every loop() iteration to advance any active fade
void updateFade() {
    if (!fade_active) return;
    uint32_t elapsed = millis() - fade_start_ms;
    if (elapsed >= fade_duration) {
        setBrightness(fade_target);
        current_brightness = fade_target;
        fade_active = false;
        return;
    }
    float t = elapsed / (float)fade_duration;
    // Ease in-out (smoothstep)
    t = t * t * (3.0f - 2.0f * t);
    uint8_t val = (uint8_t)(fade_start + t * ((int)fade_target - (int)fade_start) + 0.5f);
    if (val != current_brightness) {
        setBrightness(val);
        current_brightness = val;
    }
}

// Touch brightness override — disables auto-brightness for 10 s after any touch
#define TOUCH_OVERRIDE_MS       10000UL
static bool     touchOverride      = false;
static uint32_t touchOverrideStart = 0;

// Convert wind direction angle (0-360) to cardinal direction string
extern "C" const char* angleToCardinal(float angle) {
    // Normalize angle to 0-360 range
    while (angle < 0.0f) angle += 360.0f;
    while (angle >= 360.0f) angle -= 360.0f;
    
    // 16-point compass rose
    if (angle < 11.25f) return "N";
    if (angle < 33.75f) return "NNE";
    if (angle < 56.25f) return "NE";
    if (angle < 78.75f) return "ENE";
    if (angle < 101.25f) return "E";
    if (angle < 123.75f) return "ESE";
    if (angle < 146.25f) return "SE";
    if (angle < 168.75f) return "SSE";
    if (angle < 191.25f) return "S";
    if (angle < 213.75f) return "SSW";
    if (angle < 236.25f) return "SW";
    if (angle < 258.75f) return "WSW";
    if (angle < 281.25f) return "W";
    if (angle < 303.75f) return "WNW";
    if (angle < 326.25f) return "NW";
    if (angle < 348.75f) return "NNW";
    return "N";
}

void notifyTouch() {
    if (!touchOverride && current_brightness != 255) {
        fadeBrightness(255, 200);   // quick 200ms fade to full on touch
        Serial.println("Touch override: brightness -> 255, auto-brightness paused 10s");
    }
    touchOverride      = true;
    touchOverrideStart = millis();
    
    // Switch to menu screen on touch
    //loadScreen(SCREEN_ID_MENU);
}


// MPU-9250 — 9-axis IMU for display orientation detection
MPU9250_WE mpu(&sensorI2C, 0x68);
static uint8_t current_rotation    = 0;       // 0/1/2/3 → 0°/90°/180°/270°



void updateBrightness() {
    // Expire touch override after 10 s, then resume auto-brightness
    if (touchOverride && (millis() - touchOverrideStart >= TOUCH_OVERRIDE_MS)) {
        touchOverride = false;
        Serial.println("Touch override expired, resuming auto-brightness");
    }
    if (touchOverride) return;

    // Set I2C timeout to prevent blocking indefinitely
    static uint32_t lastBrightnessCheck = 0;
    if (millis() - lastBrightnessCheck < 500) return;  // Additional safety throttle
    lastBrightnessCheck = millis();
    
    float lux = myLux.getLux();
    //Serial.printf("BH1750 raw lux: %.1f\n", lux);
    if (lux < 0) {
        // Negative lux indicates I2C error; reset sensor
        Serial.println("WARN: BH1750 I2C error, reinitializing...");
        myLux.powerOn();
        myLux.setContHigh2Res();
        return;
    }
    bh1750_lux = lux;

    uint8_t target = luxToBrightness(lux);
    if (abs((int)target - (int)current_brightness) > 3) {
        fadeBrightness(target, 500);   // 500ms fade on auto-brightness changes
        Serial.printf("BH1750 %.1f lux → brightness %d → %d\n", lux, current_brightness, target);
    }
}


void cbSyncTime(struct timeval *tv) {
  //Serial.println("NTP time synched");
  getLocalTime(&timeinfo);
  time_t rawtime;
  struct tm* timeinfo;
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  //Serial.println(asctime(timeinfo));
  time_t now = time(nullptr);
  localTimeUnix = static_cast<uint32_t>(now);
  isSetNtp = true;
}

void initSNTP() {  
  sntp_set_sync_interval(10 * 60 * 1000UL);
  sntp_set_time_sync_notification_cb(cbSyncTime);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "192.168.50.197");
  esp_sntp_init();
  setTimezone();
}

void setTimezone() {  
  setenv("TZ","EST5EDT,M3.2.0,M11.1.0",1);
  tzset();
}


#define WIND_DIRECTIONS  16
#define WIND_SPEED_BINS   5
#define WIND_MINUTE_MAX 180    // 24 h × 60 min
 
#define every(interval) \
    static uint32_t __every__##interval = millis(); \
    if (millis() - __every__##interval >= interval && (__every__##interval = millis()))
 
// One entry per committed minute.  Only 3 bytes of data per slot + 4 bytes ts
// = 7 bytes × 1440 = ~10 KB, fully static, no heap needed.
struct MinuteWindPoint {
    uint8_t  dirBin;   // 0–15  pre-computed direction bucket
    uint8_t  spdBin;   // 0–4   pre-computed speed bucket
    uint32_t ts;       // unix timestamp of this minute's start (seconds)
};
 
static MinuteWindPoint minuteRing[WIND_MINUTE_MAX];
static int  minuteHead  = 0;   // next write slot (oldest when full)
static int  minuteCount = 0;   // how many valid entries
 
// Per-minute accumulator — circular components for direction, linear for speed
static float    accSinSum    = 0.0f;
static float    accCosSum    = 0.0f;
static float    accSpeedSum  = 0.0f;
static float    accGustSum   = 0.0f;
static int      accCount     = 0;
static uint32_t accMinute    = 0;         // current minute bucket (unix_ts / 60)
static uint32_t firstSampleTs = 0;        // unix ts of very first raw sample
 
bool windRoseDirty  = false;
bool windRoseUseGust = false;   // if true, speed bins use gust average
 
// ── Speed bin helper ─────────────────────────────────────────────────────────
static int speedToBin(float kph) {
    if (kph <  5.0f) return 0;
    if (kph < 10.0f) return 1;
    if (kph < 15.0f) return 2;
    if (kph < 20.0f) return 3;
    return 4;
}
 
// ── Flush current accumulator minute into the ring buffer ────────────────────
// Called automatically when the unix minute changes.  O(1).
static void flushAccumulatorMinute() {
    if (accCount == 0) return;
 
    float dir = atan2f(accSinSum, accCosSum) * (180.0f / (float)M_PI);
    if (dir < 0.0f) dir += 360.0f;
 
    float spd = windRoseUseGust
                    ? (accGustSum  / accCount)
                    : (accSpeedSum / accCount);
 
    MinuteWindPoint& slot = minuteRing[minuteHead];
    slot.dirBin = (uint8_t)((int)((dir + 11.25f) / 22.5f) % WIND_DIRECTIONS);
    slot.spdBin = (uint8_t)speedToBin(spd);
    slot.ts     = accMinute * 60;
 
    minuteHead = (minuteHead + 1) % WIND_MINUTE_MAX;
    if (minuteCount < WIND_MINUTE_MAX) minuteCount++;
 
    windRoseDirty = true;   // signal drawWindRose() that data has changed
}
 
// ── addWindData ───────────────────────────────────────────────────────────────
// Called on every MQTT angle message (same signature as before).
// Accumulates samples within the current unix minute; on minute boundary,
// commits the circular-mean direction + average speed to the ring buffer.
// O(1) — no sorting, no heap allocation, no matrix updates here.
void addWindData(float speed, float direction, float gust) {
    if (!isSetNtp) return;
 
    uint32_t nowTs   = (uint32_t)time(nullptr);
    uint32_t thisMin = nowTs / 60;
 
    // Track when the very first sample arrived (for sub-minute timestamp)
    if (firstSampleTs == 0) firstSampleTs = nowTs;
 
    if (accMinute == 0) {
        accMinute = thisMin;                // first call — latch minute
    } else if (thisMin != accMinute) {
        flushAccumulatorMinute();           // minute rolled over — commit
        accMinute   = thisMin;
        accSinSum   = 0.0f;
        accCosSum   = 0.0f;
        accSpeedSum = 0.0f;
        accGustSum  = 0.0f;
        accCount    = 0;
    }
 
    float rad    = direction * ((float)M_PI / 180.0f);
    accSinSum   += sinf(rad);
    accCosSum   += cosf(rad);
    accSpeedSum += speed;
    accGustSum  += gust;
    accCount++;
}
 


void drawWindRose() {
    if (minuteCount == 0) return;   // nothing committed yet
    if (!windRoseDirty)  return;    // nothing changed since last draw
    windRoseDirty = false;
 
    // ── 1. Rebuild the 2D frequency matrix from the ring buffer — O(1440) ──
    //    Because we rebuild from the ring, old data is automatically excluded
    //    when the ring wraps past 24 h.  The 2D matrix is just a view, never
    //    a source of truth.
    int matrix[WIND_DIRECTIONS][WIND_SPEED_BINS] = {};   // zero-init on stack
 
    int oldest = (minuteCount < WIND_MINUTE_MAX) ? 0 : minuteHead;
    for (int i = 0; i < minuteCount; i++) {
        const MinuteWindPoint& p = minuteRing[(oldest + i) % WIND_MINUTE_MAX];
        matrix[p.dirBin][p.spdBin]++;
    }
 
    // Direction totals + normalisation max
    int dirTotals[WIND_DIRECTIONS] = {};
    int maxTotal = 0;
    for (int d = 0; d < WIND_DIRECTIONS; d++) {
        for (int s = 0; s < WIND_SPEED_BINS; s++) dirTotals[d] += matrix[d][s];
        if (dirTotals[d] > maxTotal) maxTotal = dirTotals[d];
    }
    if (maxTotal == 0) return;
 
    // ── 2. Allocate canvas once, reuse forever ──────────────────────────────
    static lv_obj_t      *canvas     = NULL;
    static lv_draw_buf_t  draw_buf;
    static void          *canvas_buf = NULL;
 
    const int16_t CW = 275, CH = 275;
    const int16_t CX = CW / 2, CY = CH / 2;
    const float   MAX_R = 137.0f;
 
    const int WINDC_X  = -85, WINDC_Y = 39;
    const int DRAW_CX  = 240 + WINDC_X;
    const int DRAW_CY  = 240 + WINDC_Y;
 
    // Speed-bin colours: calm → cyan, light → green, moderate → yellow,
    //                    fresh → orange, strong → red
    static const lv_color_t speedColors[WIND_SPEED_BINS] = {
        lv_color_hex(0x00FFFF),   // < 5 kph
        lv_color_hex(0x00FF00),   // < 10 kph
        lv_color_hex(0xFFFF00),   // < 15 kph
        lv_color_hex(0xFF8800),   // < 20 kph
        lv_color_hex(0xFF0000),   // ≥ 20 kph
    };
 
    if (canvas == NULL) {
        canvas = lv_canvas_create(objects.main);
 
        uint32_t bufsz = (uint32_t)CW * CH * 4;   // ARGB8888
        canvas_buf = heap_caps_malloc(bufsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!canvas_buf) canvas_buf = malloc(bufsz);
        if (!canvas_buf) {
            Serial.println("WARN: wind rose canvas alloc failed");
            canvas = NULL;
            return;
        }
 
        lv_draw_buf_init(&draw_buf, CW, CH, LV_COLOR_FORMAT_ARGB8888,
                         0, canvas_buf, bufsz);
        lv_canvas_set_draw_buf(canvas, &draw_buf);
        lv_obj_set_pos(canvas, (DRAW_CX + 3) - CX, DRAW_CY - CY);
        lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(canvas, 0, 0);
        lv_obj_set_style_pad_all(canvas, 0, 0);
    }
 
    // ── 3. Draw — ≤ 80 arc calls total (16 dirs × 5 speed bins) ────────────
    //    LVGL arc: `radius` is the OUTER edge of the stroke; `width` extends
    //    inward from there.  So radius = outerR, width = outerR - innerR.
    //    No inner loop subdividing bands.
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
 
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
 
    for (int d = 0; d < WIND_DIRECTIONS; d++) {
        if (dirTotals[d] == 0) continue;
 
        float spokeLen = (float)dirTotals[d] / (float)maxTotal * MAX_R;
        float dirAngle = (float)d * (360.0f / WIND_DIRECTIONS) - 90.0f;
 
        // Arc spans ±11.25° around the spoke centre (= one 22.5° slice)
        int16_t startAngle = (int16_t)(dirAngle - 11.25f);
        int16_t endAngle   = (int16_t)(dirAngle + 11.25f);
        while (startAngle < 0) startAngle += 360;
        while (endAngle   < 0) endAngle   += 360;
 
        float currentR = 0.0f;
        for (int s = 0; s < WIND_SPEED_BINS; s++) {
            if (matrix[d][s] == 0) continue;
 
            float bandLen = (float)matrix[d][s] / (float)dirTotals[d] * spokeLen;
            float outerR  = currentR + bandLen;
            int   arcW    = (int)(bandLen) + 1;   // +1 closes pixel gaps between bands
            if (arcW < 2) arcW = 2;
 
            lv_draw_arc_dsc_t arc;
            lv_draw_arc_dsc_init(&arc);
            arc.color       = speedColors[s];
            arc.width       = (uint16_t)arcW;    // LVGL strokes INWARD from radius
            arc.opa         = LV_OPA_COVER;
            arc.rounded     = 0;
            arc.center.x    = CX;
            arc.center.y    = CY;
            arc.radius      = (uint16_t)(outerR + 0.5f);  // outer edge of this band
            arc.start_angle = startAngle;
            arc.end_angle   = endAngle;
 
            lv_draw_arc(&layer, &arc);
            currentR = outerR;
        }
    }
 
    lv_canvas_finish_layer(canvas, &layer);
}


// Shows how much data has accumulated:
//   - No data yet                         →  "--"
//   - < 1 committed minute (sub-minute)   →  "Xs"   (seconds since first sample)
//   - ≥ 1 committed minute                →  "Xm" or "X.Xh"
//                                             (oldest ring entry → now)
//
// This naturally bootstraps from "5s" on first boot through "24hrs" once full.
void updateWindTimestamp() {
    uint32_t nowTs = (uint32_t)time(nullptr);
 
    if (minuteCount == 0) {
        // No committed minute yet — show sub-minute accumulation time
        if (firstSampleTs == 0 || nowTs <= firstSampleTs) {
            lv_label_set_text(objects.label_time_1, "--");
        } else {
            uint32_t secs = nowTs - firstSampleTs;
            lv_label_set_text_fmt(objects.label_time_1, "%us", secs);
        }
        return;
    }
 
    // Oldest committed minute in the ring
    int      oldest   = (minuteCount < WIND_MINUTE_MAX) ? 0 : minuteHead;
    uint32_t oldestTs = minuteRing[oldest].ts;
    uint32_t spanSecs = (nowTs > oldestTs) ? (nowTs - oldestTs) : 0;
 
    if (spanSecs < 60) {
        lv_label_set_text_fmt(objects.label_time_1, "%us", spanSecs);
    } else if (spanSecs < 3600) {
        lv_label_set_text_fmt(objects.label_time_1, "%um", spanSecs / 60);
    } else {
        lv_label_set_text_fmt(objects.label_time_1, "%.1fh", spanSecs / 3600.0f);
    }
}

class LGFX : public lgfx::LGFX_Device
{
public:

  lgfx::Bus_RGB      _bus_instance;
  lgfx::Panel_ST7701_guition_esp32_4848S040 _panel_instance;
  lgfx::Touch_GT911  _touch_instance;
  lgfx::Light_PWM   _light_instance;

  LGFX(void)
  {
    {
      auto cfg = _panel_instance.config();

      cfg.memory_width  = 480;
      cfg.memory_height = 480;
      cfg.panel_width  = 480;
      cfg.panel_height = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config_detail();

      cfg.pin_cs = 39;
      cfg.pin_sclk = 48;
      cfg.pin_mosi = 47; // SDA

      _panel_instance.config_detail(cfg);
    }

    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      cfg.pin_d0  = GPIO_NUM_4;  // B0
      cfg.pin_d1  = GPIO_NUM_5;  // B1
      cfg.pin_d2  = GPIO_NUM_6; // B2
      cfg.pin_d3  = GPIO_NUM_7; // B3
      cfg.pin_d4  = GPIO_NUM_15;  // B4
      cfg.pin_d5  = GPIO_NUM_8;  // G0
      cfg.pin_d6  = GPIO_NUM_20;  // G1
      cfg.pin_d7  = GPIO_NUM_3; // G2
      cfg.pin_d8  = GPIO_NUM_46; // G3
      cfg.pin_d9  = GPIO_NUM_9; // G4
      cfg.pin_d10 = GPIO_NUM_10;  // G5
      cfg.pin_d11 = GPIO_NUM_11; // R0
      cfg.pin_d12 = GPIO_NUM_12; // R1
      cfg.pin_d13 = GPIO_NUM_13; // R2
      cfg.pin_d14 = GPIO_NUM_14; // R3
      cfg.pin_d15 = GPIO_NUM_0; // R4

      cfg.pin_henable = GPIO_NUM_18;
      cfg.pin_vsync   = GPIO_NUM_17;
      cfg.pin_hsync   = GPIO_NUM_16;
      cfg.pin_pclk    = GPIO_NUM_21;
      cfg.freq_write  = 8000000;

      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 10;
      cfg.hsync_pulse_width = 8;
      cfg.hsync_back_porch  = 50;
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 10;
      cfg.vsync_pulse_width = 8;
      cfg.vsync_back_porch  = 20;
      cfg.pclk_idle_high    = 0;
      cfg.de_idle_high      = 1;
      _bus_instance.config(cfg);
    }
    _panel_instance.setBus(&_bus_instance);

    // NOTE: Light instance disabled — backlight handled manually via LEDC in setup()
    // to support dual GPIO control (GPIO38 EN + GPIO2 FB)
    // {
    //   auto cfg = _light_instance.config();
    //   cfg.pin_bl = GPIO_NUM_38;
    //   _light_instance.config(cfg);
    // }
    // _panel_instance.light(&_light_instance);

    {
      auto cfg = _touch_instance.config();
      cfg.x_min      = 0;
      cfg.x_max      = 480;
      cfg.y_min      = 0;
      cfg.y_max      = 480;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;

      cfg.i2c_port   = I2C_NUM_1;

      cfg.pin_int    = GPIO_NUM_NC;
      cfg.pin_sda    = GPIO_NUM_19;
      cfg.pin_scl    = GPIO_NUM_45;
      cfg.pin_rst    = GPIO_NUM_NC;
      cfg.i2c_addr   = 0x5D;   // GT911 address on this board (INT/RST held high during boot)

      cfg.freq       = 400000;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};
static LGFX lcd;

// ==================== LVGL Integration ====================
static uint32_t screenWidth = 480;
static uint32_t screenHeight = 480;

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    lcd.pushImage(area->x1, area->y1, w, h, (uint16_t *)px_map); 
    lv_disp_flush_ready(disp);
}
// ==================== Touch (LovyanGFX built-in GT911) ====================
// Touch_GT911 is already configured inside the LGFX class (I2C_NUM_1, SDA=19, SCL=45).
// lcd.getTouch() returns true while a finger is down and fills x/y in screen pixels.

static int32_t touch_x = 0;
static int32_t touch_y = 0;

void my_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t tx = 0, ty = 0;
    if (lcd.getTouch(&tx, &ty)) {
        touch_x = tx;
        touch_y = ty;
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state    = LV_INDEV_STATE_PRESSED;
        Serial.printf("Touch: x=%d  y=%d\n", touch_x, touch_y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}



// ==================== Sensor Functions ====================

void initSensors() {
    // Start the dedicated sensor I2C bus on GPIO1 (SDA) and GPIO40 (SCL)
    sensorI2C.begin(1, 40);

    // --- BH1750 ---
    myLux.powerOn();
    Serial.println("BH1750 initialised");

    myLux.setContHigh2Res();
    // --- MPU-9250 ---
    if (mpu.init()) {
        Serial.println("MPU-9250 initialised");
        // Set gyro offsets manually (tune these from serial output if needed, 
        // or just use zeros — gyro drift doesn't matter for static orientation)
        mpu.setGyrOffsets(0.0, 0.0, 0.0);
        // Do NOT call mpu.autoOffsets() at all
        mpu.setAccRange(MPU9250_ACC_RANGE_2G);
        mpu.enableAccDLPF(true);
        mpu.setAccDLPF(MPU9250_DLPF_6);
    } else {
        Serial.println("WARN: MPU-9250 not found — rotation locked at 0°");
    }
}

// Map lux → unified 0–255 brightness scale.
// 0.0 lux = dimmest still-visible (0), 5.0 lux = full brightness (255).
static uint8_t luxToBrightness(float lux) {
    if (lux <= 0.0f) return 0;
    if (lux >= 5.0f) return 255;
    // Logarithmic mapping over 0.0 – 5.0 lux
    float norm = log10f(lux + 1.0f) / log10f(6.0f);  // 0.0 – 1.0
    return (uint8_t)constrain((int)(norm * 255.0f + 0.5f), 0, 255);
}



// ==================== Orientation Detection ====================
// The SparkFun MPU-9250 is glued flat against the display panel, so the
// display is always wall-mounted (never truly flat).  Gravity falls along
// X or Y depending on which edge is down; Z is always perpendicular to the
// display face and carries very little gravity.
//
// getOrientation() enum -> display rotation mapping
// (tune by reading serial output while holding each edge down):
//   YX    -> top edge up   -> rotation 0   (normal portrait)
//   XY_1  -> right edge up -> rotation 1   (landscape CW)
//   YX_1  -> bottom up     -> rotation 2   (portrait upside-down)
//   XY    -> left edge up  -> rotation 3   (landscape CCW)
//   FLAT / FLAT_1 -> display lying flat, keep last rotation
//
// If the serial log shows a different enum for a given physical orientation,
// swap the case values in orientationToRotation() below.

static const char* orientationName(MPU9250_orientation o) {
    switch (o) {
        case MPU9250_FLAT:   return "FLAT";
        case MPU9250_FLAT_1: return "FLAT_1";
        case MPU9250_XY:     return "XY";
        case MPU9250_XY_1:   return "XY_1";
        case MPU9250_YX:     return "YX";
        case MPU9250_YX_1:   return "YX_1";
        default:             return "UNKNOWN";
    }
}

// Returns 0-3 rotation, or 0xFF if orientation is ambiguous (flat).
static uint8_t orientationToRotation(MPU9250_orientation o) {
    switch (o) {
        case MPU9250_YX:     return 0;    // top up    -> normal
        case MPU9250_XY_1:   return 3;    // right up  -> 270 CCW
        case MPU9250_YX_1:   return 2;    // upside-down
        case MPU9250_XY:     return 1;    // left up   -> 90 CW
        default:             return 0xFF; // FLAT / FLAT_1 - ignore
    }
}

// Apply rotation to lcd + LVGL.  forceApply skips the change-detection
// guard so it works correctly on first boot.
static void applyRotation(uint8_t rot, bool forceApply = false) {
    if (!forceApply && rot == current_rotation) return;
    current_rotation = rot;
    Serial.printf("  -> Applying display rotation %d deg\n", rot * 90);
    lcd.setRotation(rot);
    uint32_t w = lcd.width();
    uint32_t h = lcd.height();
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_set_resolution(disp, w, h);
        lv_obj_invalidate(lv_scr_act());
    }
}

// Called every 500 ms from loop() to track live rotation changes.
void updateOrientation() {
    MPU9250_orientation orient = mpu.getOrientation();
    uint8_t new_rotation = orientationToRotation(orient);

    if (new_rotation != 0xFF)
        applyRotation(new_rotation);
}

// Called once in setup() after initSensors() and lcd.init() but before ui_init().
// Reads orientation and immediately rotates the display to match.
void applyBootOrientation() {
    delay(50); // let sensor settle after init
    MPU9250_orientation orient = mpu.getOrientation();
    uint8_t rot = orientationToRotation(orient);
    Serial.printf("Boot orientation: %s", orientationName(orient));
    if (rot == 0xFF) {
        rot = 0; // display is flat on a desk - default to rotation 0
        Serial.println(" -> flat, defaulting to 0 deg");
    } else {
        Serial.printf(" -> rotation %d deg\n", rot * 90);
    }
    applyRotation(rot, true); // force-apply even if rot == current_rotation
}

// =====================================================================
// WEATHER RADAR  (tile.openstreetmap.org + tilecache.rainviewer.com)
//
// Requires these libraries (Arduino Library Manager):
//   • PNGdec  by Larry Bank (bitbank2)
//   • ArduinoJson
// LV_USE_PNG is NOT required – all PNG decoding is done via PNGdec.
//
// Memory: 4 canvas pixel buffers × 256 KB = 1 MB PSRAM.
// Tile layout inside radar_cont (512 × 512):
//   [0] (0,  0 ) → z,  radarX,   radarY
//   [1] (256,0 ) → z,  radarX+1, radarY
//   [2] (0,  256) → z, radarX,   radarY+1
//   [3] (256,256) → z, radarX+1, radarY+1
// =====================================================================

// ---- tile state (top-left tile of 2×2 grid) ----
static int   radarZ = 7, radarX = 34, radarY = 46;
static uint32_t      radarLastFetch   = 0;
static unsigned long radarTimestamp   = 0;   // unix ts of latest radar frame

// ---- cross-file flags (extern'd in screens.c) ----
bool radarForceRefresh = true;
bool radarScreenActive = false;

// ---- LVGL canvas objects (4 tiles, 256×256 ARGB8888 each) ----
static lv_obj_t*     radarCanvas[4]   = {};
static lv_draw_buf_t radarDrawBuf[4];
static uint32_t*     radarPixBuf[4]   = {};   // PSRAM; 256 KB each
static lv_obj_t*     radarDot         = nullptr;
static bool          radarReady       = false;

// ---- PNGdec globals (decoder uses a single-call callback model) ----
static PNG       pngDec;
static uint32_t* gDecBuf     = nullptr;   // destination pixel buffer for current decode
static bool      gDecOverlay = false;     // true → alpha-composite; false → opaque write

// One scanline at a time from PNGdec.
static int pngRow(PNGDRAW* pDraw) {
    if (!gDecBuf || pDraw->y >= 256 || pDraw->iWidth > 256) return 1;
    uint32_t* dest = gDecBuf + (uint32_t)pDraw->y * 256;

    if (gDecOverlay && pDraw->iHasAlpha && pDraw->iPixelType == 6) {
        // ---- Radar overlay: RGBA truecolor, 4 bytes/pixel (R,G,B,A) ----
        // Alpha-composite ("src over") onto the map pixels already in the buffer.
        const uint8_t* s = (const uint8_t*)pDraw->pPixels;
        for (int x = 0; x < pDraw->iWidth; x++, s += 4) {
            uint8_t a = s[3];
            if (a == 0) continue;               // fully transparent → keep map pixel
            uint8_t r = s[0], g = s[1], b = s[2];
            if (a < 255) {
                uint32_t bg = dest[x];
                uint8_t bgR = (bg >> 16) & 0xFF;
                uint8_t bgG = (bg >>  8) & 0xFF;
                uint8_t bgB =  bg        & 0xFF;
                r = (uint8_t)(((uint16_t)r * a + (uint16_t)bgR * (255u - a)) >> 8);
                g = (uint8_t)(((uint16_t)g * a + (uint16_t)bgG * (255u - a)) >> 8);
                b = (uint8_t)(((uint16_t)b * a + (uint16_t)bgB * (255u - a)) >> 8);
            }
            // LVGL ARGB8888 on LE: uint32 = (A<<24)|(R<<16)|(G<<8)|B
            dest[x] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    } else {
        // ---- Map tile: RGB or palette source → convert via RGB565 helper ----
        uint16_t line[256];
        pngDec.getLineAsRGB565(pDraw, line, PNG_RGB565_LITTLE_ENDIAN, 0);
        for (int x = 0; x < pDraw->iWidth; x++) {
            uint16_t p = line[x];
            // Expand 5/6/5 → 8/8/8 with bit-replication for full range
            uint8_t r = ((p >> 11) & 0x1F); r = (r << 3) | (r >> 2);
            uint8_t g = ((p >>  5) & 0x3F); g = (g << 2) | (g >> 4);
            uint8_t b = ( p        & 0x1F); b = (b << 3) | (b >> 2);
            dest[x] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    return 1;
}

// Allocate 4 LVGL canvas widgets inside radar_cont and move UI chrome to front.
// Must be called after create_screens() so objects.radar_cont already exists.
void initRadarCanvases() {
    if (radarReady) return;
    const size_t TILE_BYTES = 256u * 256u * 4u;   // 256 KB per tile → 1 MB total

    for (int i = 0; i < 4; i++) {
        radarPixBuf[i] = (uint32_t*)heap_caps_malloc(
                            TILE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!radarPixBuf[i]) {
            Serial.printf("RADAR: PSRAM alloc failed for canvas %d\n", i);
            return;
        }
        memset(radarPixBuf[i], 0x00, TILE_BYTES);   // transparent black until first fetch

        radarCanvas[i] = lv_canvas_create(objects.radar_cont);
        int cx = (i & 1) ? 256 : 0;
        int cy = (i & 2) ? 256 : 0;
        lv_obj_set_pos(radarCanvas[i], cx, cy);
        lv_obj_set_style_pad_all(radarCanvas[i], 0, 0);
        lv_obj_set_style_border_width(radarCanvas[i], 0, 0);
        lv_obj_set_style_bg_opa(radarCanvas[i], LV_OPA_TRANSP, 0);

        lv_draw_buf_init(&radarDrawBuf[i], 256, 256, LV_COLOR_FORMAT_ARGB8888,
                         0, radarPixBuf[i], TILE_BYTES);
        lv_canvas_set_draw_buf(radarCanvas[i], &radarDrawBuf[i]);
    }

    // Location marker: 8×8 red circle with white outline
    radarDot = lv_obj_create(objects.radar_cont);
    lv_obj_set_size(radarDot, 8, 8);
    lv_obj_set_style_radius(radarDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(radarDot, lv_color_hex(0xFF2020), 0);
    lv_obj_set_style_bg_opa(radarDot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(radarDot, 1, 0);
    lv_obj_set_style_border_color(radarDot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(radarDot, 0, 0);
    lv_obj_add_flag(radarDot, LV_OBJ_FLAG_HIDDEN);

    // Keep EXIT button and info labels rendered above the map canvases
    lv_obj_move_foreground(objects.obj1);
    lv_obj_move_foreground(objects.label_timestamp);
    lv_obj_move_foreground(objects.label_coords);

    radarReady = true;
    Serial.printf("RADAR: ready – 4×256KB canvas in PSRAM (%u KB free)\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

// Show a full-screen error message directly via LovyanGFX (bypasses LVGL),
// then hold for 2 s so the user can read it before LVGL repaints.
static void radarError(const char* msg) {
    Serial.printf("RADAR ERROR: %s\n", msg);
    lcd.fillRect(0, 160, screenWidth, 160, TFT_BLACK);
    lcd.setTextColor(TFT_RED, TFT_BLACK);
    lcd.setTextSize(3);
    lcd.setTextDatum(MC_DATUM);
    lcd.drawString(msg, screenWidth / 2, 240);
    lcd.setTextDatum(TL_DATUM);   // restore default
    delay(2000);
}

// Download a URL into a fresh PSRAM buffer.  Retries up to 3 times on failure.
// Returns nullptr on all failures; caller must heap_caps_free() the result.
static uint8_t* fetchTile(WiFiClientSecure& sc, const String& url, int& outLen) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        HTTPClient http;
        http.begin(sc, url);
        http.setTimeout(12000);
        http.addHeader("User-Agent", "ESP32-Weather/1.0");
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            http.end();
            char msg[64];
            snprintf(msg, sizeof(msg), "Tile HTTP %d (try %d/3)", code, attempt);
            radarError(msg);
            delay(500);
            continue;
        }
        int len = http.getSize();
        if (len <= 0 || len > 256 * 1024) {
            http.end();
            char msg[64];
            snprintf(msg, sizeof(msg), "Bad length %d (try %d/3)", len, attempt);
            radarError(msg);
            delay(500);
            continue;
        }
        uint8_t* buf = (uint8_t*)heap_caps_malloc(len,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            http.end();
            radarError("PSRAM alloc failed");
            outLen = 0; return nullptr;   // no point retrying an OOM
        }
        WiFiClient* stream = http.getStreamPtr();
        int got = 0;
        uint32_t t0 = millis();
        while (got < len && millis() - t0 < 12000) {
            int avail = stream->available();
            if (avail > 0) got += stream->readBytes(buf + got,
                                  (int)min((uint32_t)avail, (uint32_t)(len - got)));
            else delay(1);
        }
        http.end();
        if (got != len) {
            heap_caps_free(buf);
            char msg[64];
            snprintf(msg, sizeof(msg), "Short read %d/%d (try %d/3)", got, len, attempt);
            radarError(msg);
            delay(500);
            continue;
        }
        outLen = len;
        return buf;
    }
    outLen = 0; return nullptr;
}

// Query the RainViewer API for the most recent radar frame path.
// Retries up to 3 times on failure.
static String fetchRadarPath() {
    for (int attempt = 1; attempt <= 3; attempt++) {
        HTTPClient http;
        http.begin("https://api.rainviewer.com/public/weather-maps.json");
        http.setTimeout(8000);
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            http.end();
            char msg[64];
            snprintf(msg, sizeof(msg), "API HTTP %d (try %d/3)", code, attempt);
            radarError(msg);
            delay(500);
            continue;
        }
        String json = http.getString();
        http.end();

        JsonDocument doc;
        if (deserializeJson(doc, json) != DeserializationError::Ok) {
            radarError("JSON parse failed");
            delay(500);
            continue;
        }
        JsonArray past = doc["radar"]["past"];
        if (past.size() == 0) {
            radarError("No radar data");
            delay(500);
            continue;
        }
        JsonObject latest = past[past.size() - 1];
        radarTimestamp = latest["time"].as<unsigned long>();
        return latest["path"].as<String>();
    }
    return "";
}

// Decode one PNG buffer into the pixel buffer for canvas slot `idx`.
// isOverlay=false → opaque write (map base).
// isOverlay=true  → alpha-composite over existing pixels (radar layer).
static void decodeIntoCanvas(uint8_t* data, int len, int idx, bool isOverlay) {
    gDecBuf     = radarPixBuf[idx];
    gDecOverlay = isOverlay;
    if (pngDec.openRAM(data, len, pngRow) == PNG_SUCCESS) {
        pngDec.decode(nullptr, 0);
        pngDec.close();
    } else {
        Serial.printf("RADAR: PNG open failed for slot %d\n", idx);
    }
}

// Main radar refresh.  Fetches 4 map tiles + 4 radar overlays, decodes them
// into the canvas pixel buffers, then invalidates the canvases so LVGL redraws.
// Blocking (several seconds) – called from loop() only when on the radar screen.
void doRadar() {
    if (!radarReady)          return;
    if (!WiFi.isConnected())  return;
    if (!radarForceRefresh && millis() - radarLastFetch < 300000UL) return;

    // Yield to LVGL *before* we block on network I/O so that button pressed
    // states (and any other pending redraws) are flushed to the display first.
    // Without this, the user sees no visual feedback until the download finishes.
    lv_timer_handler();
    delay(30);   // allow DMA display transfer to complete
    lv_timer_handler();

    radarLastFetch    = millis();
    radarForceRefresh = false;

    int z = radarZ, x = radarX, y = radarY;
    int maxTile = (1 << z) - 1;

    // Four tile coordinates: [0]=(x,y) [1]=(x+1,y) [2]=(x,y+1) [3]=(x+1,y+1)
    int tx[4] = { x,     x + 1, x,     x + 1 };
    int ty[4] = { y,     y,     y + 1, y + 1 };
    for (int i = 0; i < 4; i++) {
        tx[i] = constrain(tx[i], 0, maxTile);
        ty[i] = constrain(ty[i], 0, maxTile);
    }

    // Helper: draw (or erase) the progress bar directly via LovyanGFX,
    // bypassing LVGL entirely so it appears immediately on screen.
    // pct 0..100; colour 0 = erase (black).
    auto radarProgress = [&](int pct, uint32_t colour) {
        int barW = (screenWidth * pct) / 100;
        // Erase full bar row first, then fill to barW
        lcd.fillRect(0, 470, screenWidth, 10, 0x000000u);
        if (barW > 0 && colour != 0) {
            lcd.fillRect(0, 470, barW, 10, colour);
        }
    };

    // ---- Step 1: latest radar path (RainViewer API) ----
    String radarPath = fetchRadarPath();
    if (radarPath.isEmpty()) {
        lv_label_set_text(objects.label_timestamp, "Radar: API error");
        radarProgress(0, 0);
        return;
    }
    radarProgress(20, 0x00AA00u);   // 20% — got radar path

    // Single reusable SSL client for all 8 tile fetches
    WiFiClientSecure sc;
    sc.setInsecure();

    // ---- Steps 2+3: for each quadrant, download map then blend radar ----
    for (int i = 0; i < 4; i++) {
        // Map tile (OpenStreetMap)
        String mapUrl = String("https://tile.openstreetmap.org/")
                      + z + "/" + tx[i] + "/" + ty[i] + ".png";
        int mapLen = 0;
        uint8_t* mapBuf = fetchTile(sc, mapUrl, mapLen);
        if (mapBuf) {
            decodeIntoCanvas(mapBuf, mapLen, i, false);
            heap_caps_free(mapBuf);
        }
        delay(30);   // brief yield so SSL session can settle

        // Radar overlay (RainViewer) – transparent RGBA PNG composited on top
        String radUrl = String("https://tilecache.rainviewer.com")
                      + radarPath + "/256/" + z + "/"
                      + tx[i] + "/" + ty[i] + "/2/0_1.png";
        int radLen = 0;
        uint8_t* radBuf = fetchTile(sc, radUrl, radLen);
        if (radBuf) {
            decodeIntoCanvas(radBuf, radLen, i, true);
            heap_caps_free(radBuf);
        }
        delay(30);

        lv_obj_invalidate(radarCanvas[i]);   // queue LVGL redraw for this tile

        // 40 / 60 / 80 / 100% as each tile completes
        radarProgress(40 + i * 20, 0x00AA00u);
    }

    radarProgress(0, 0);   // erase bar — we're done

    // ---- Step 4: location marker (43°22′30.7″N, 80°56′44.2″W) ----
    lv_obj_add_flag(radarDot, LV_OBJ_FLAG_HIDDEN);
    const double LOC_LAT =  43.375194;
    const double LOC_LON = -80.945611;
    double lat_r = LOC_LAT * M_PI / 180.0;
    double n     = pow(2.0, z);
    double xtile = n * ((LOC_LON + 180.0) / 360.0);
    double ytile = n * (1.0 - log(tan(lat_r) + 1.0 / cos(lat_r)) / M_PI) / 2.0;
    for (int i = 0; i < 4; i++) {
        if ((int)xtile == tx[i] && (int)ytile == ty[i]) {
            int px = (int)((xtile - (int)xtile) * 256.0);
            int py = (int)((ytile - (int)ytile) * 256.0);
            int qx = (i & 1) ? 256 : 0;
            int qy = (i & 2) ? 256 : 0;
            lv_obj_set_pos(radarDot, qx + px - 4, qy + py - 4);   // centre the 8×8 dot
            lv_obj_clear_flag(radarDot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(radarDot);
            break;
        }
    }

    // ---- Step 5: update info labels ----
    if (radarTimestamp > 0) {
        time_t ts = (time_t)radarTimestamp;
        struct tm* ti = localtime(&ts);
        char buf[48];
        strftime(buf, sizeof(buf), "Radar: %b %d, %I:%M %p", ti);
        lv_label_set_text(objects.label_timestamp, buf);
    }
    lv_label_set_text_fmt(objects.label_coords, "X-%d, Y-%d [Z-%d]", x, y, z);
    Serial.printf("RADAR: refresh done z=%d x=%d y=%d\n", z, x, y);
}

// =====================================================================
// RADAR ACTION HANDLERS
// extern "C" so that the C-compiled screens.c can call them via actions.h.
// =====================================================================

extern "C" void action_radar_up(lv_event_t* e) {
    if (radarY > 0) radarY--;
    radarForceRefresh = true;
}
extern "C" void action_radar_down(lv_event_t* e) {
    int maxTile = (1 << radarZ) - 2;   // -2: bottom tile is radarY+1
    if (radarY < maxTile) radarY++;
    radarForceRefresh = true;
}
extern "C" void action_radar_left(lv_event_t* e) {
    if (radarX > 0) radarX--;
    radarForceRefresh = true;
}
extern "C" void action_radar_righ(lv_event_t* e) {
    int maxTile = (1 << radarZ) - 2;   // -2: right tile is radarX+1
    if (radarX < maxTile) radarX++;
    radarForceRefresh = true;
}
extern "C" void action_radar_right(lv_event_t* e) {
    action_radar_righ(e);   // both symbol names are registered in screens.c
}
extern "C" void action_radar_in(lv_event_t* e) {
    if (radarZ >= 10) return;
    // Preserve the geographic centre of the 2×2 grid across the zoom
    double n0 = pow(2.0, radarZ);
    double lon = (radarX + 1.0) / n0 * 360.0 - 180.0;
    double lr  = atan(sinh(M_PI * (1.0 - 2.0 * (radarY + 1.0) / n0)));
    double lat = lr * 180.0 / M_PI;
    radarZ++;
    double n1 = pow(2.0, radarZ);
    double lr2 = lat * M_PI / 180.0;
    radarX = constrain((int)(n1 * ((lon + 180.0) / 360.0)) - 1, 0, (1 << radarZ) - 2);
    radarY = constrain((int)(n1 * (1.0 - log(tan(lr2) + 1.0/cos(lr2))/M_PI)/2.0) - 1,
                       0, (1 << radarZ) - 2);
    radarForceRefresh = true;
}
extern "C" void action_radar_out(lv_event_t* e) {
    if (radarZ <= 1) return;
    double n0 = pow(2.0, radarZ);
    double lon = (radarX + 1.0) / n0 * 360.0 - 180.0;
    double lr  = atan(sinh(M_PI * (1.0 - 2.0 * (radarY + 1.0) / n0)));
    double lat = lr * 180.0 / M_PI;
    radarZ--;
    int maxTile = max(0, (1 << radarZ) - 2);
    double n1 = pow(2.0, radarZ);
    double lr2 = lat * M_PI / 180.0;
    radarX = constrain((int)(n1 * ((lon + 180.0) / 360.0)) - 1, 0, maxTile);
    radarY = constrain((int)(n1 * (1.0 - log(tan(lr2) + 1.0/cos(lr2))/M_PI)/2.0) - 1,
                       0, maxTile);
    radarForceRefresh = true;
}
extern "C" void action_radar_indddddddd(lv_event_t* e) { /* auto-generated stub */ }

bool connected = false;

void setup()
{
  Serial.begin(115200);
  Serial.println("\n\nStarting LVGL Display with LovyanGFX and GT911 Touch");
  
  // Create FreeRTOS queue for MQTT handoff
  mqttQueue = xQueueCreate(10, sizeof(MqttPending));
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); //low power for better connectivity


  Serial.println("Wind rose 2D matrix initialized (320 bytes total)");
  
  Serial.println("Initializing display...");

  lcd.init();
  delay(50);

  Serial.println("Display initialized");

  initSensors();
  applyBootOrientation();

  lv_init();
    // In setup(), after lv_init():
    xTaskCreatePinnedToCore(
        [](void*) {
            for (;;) {
                lv_tick_inc(1);
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        },
        "lv_tick", 2048, NULL, configMAX_PRIORITIES - 1, NULL, 1  // pin to Core 1
    );
  static lv_display_t *disp = lv_display_create(screenWidth, screenHeight);

// =========================================================================
  // FIX: Double-Buffered Internal SRAM to prevent PSRAM/RGB starvation
  // 40 lines = 38.4 KB per buffer. Two buffers = ~76.8 KB total.
  // This easily fits in the ESP32-S3's 512KB internal RAM.
  // =========================================================================
  const uint32_t buf_lines = 10; 
  static uint8_t *buf1 = (uint8_t *)heap_caps_malloc(screenWidth * buf_lines * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  static uint8_t *buf2 = (uint8_t *)heap_caps_malloc(screenWidth * buf_lines * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!buf1 || !buf2) {
    Serial.println("ERROR: Could not allocate internal display buffers!");
    while (1) delay(1000);
  } else {
    Serial.println("Allocated dual display buffers from fast Internal SRAM");
  }

  // Pass BOTH buffers to LVGL. This allows true DMA ping-ponging.
  lv_display_set_buffers(disp, buf1, buf2, screenWidth * buf_lines * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_resolution(disp, screenWidth, screenHeight);
  lv_display_set_default(disp);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read);

  ui_init();
  Serial.println("UI initialized");

  initRadarCanvases();

  Serial.println("Setup complete - Display and touch ready!");
  // Setup backlight LEDC (dual GPIO mode)
  // Both channels use same frequency and resolution to share LEDC timer
  // Attach once to avoid conflicts with LGFX panel initialization
  ledcAttach(GFX_BL,    BL_PWM_FREQ, BL_PWM_RESOLUTION);
  ledcAttach(GFX_BL_FB, BL_PWM_FREQ, BL_PWM_RESOLUTION);
  fadeBrightness(255, 0);   // start at full brightness instantly
  Serial.println("Backlight initialised (dual GPIO mode)");

  // ── MQTT client setup (timer + callbacks). connectToMqtt() is called
  //    once WiFi is up, from the first-connect block in loop(). ──
  mqttReconnectTimer = xTimerCreate("mqttReconnect", pdMS_TO_TICKS(15000),
                                    pdFALSE, NULL,
                                    [](TimerHandle_t){ connectToMqtt(); });
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setClientId(mqttClientId);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        if (connected) {
            Serial.println("WiFi disconnected");
            connected = false;
            WiFi.reconnect();
        }
    } else {
        if (!connected) {
            Serial.println("\nWiFi connected");
            Serial.print("IP address: ");
            Serial.println(WiFi.localIP());
            Serial.print("RSSI: ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");
            connectToMqtt();
            initSNTP();
            ArduinoOTA.setHostname("monolith");
            ArduinoOTA.begin();
            connected = true;
        }
        else {
            ArduinoOTA.handle();
        }
    }
    
    lv_timer_handler();
    ui_tick();
    
    applyMqttPending();   // drain MQTT data from AsyncTCP task → LVGL/windHistory
    updateFade();   // advance any active brightness fade

    // Radar: blocking tile fetch; returns immediately if not due or not on screen
    if (radarScreenActive) doRadar();
    
    every(1000) {
        updateWindTimestamp();  // update elapsed time label
    }

    // --- Raw touch diagnostic (remove once touch is confirmed working) ---
    static bool lastTouched = false;
    uint16_t tx, ty;
    bool touched = lcd.getTouch(&tx, &ty);
    if (touched && !lastTouched) {
        Serial.printf("RAW touch DOWN: x=%d y=%d\n", tx, ty);
        notifyTouch();
    } else if (!touched && lastTouched) {
        Serial.println("RAW touch UP");
    }
    lastTouched = touched;
    
    // Auto-brightness from BH1750 and orientation tracking
    every(500) {
        updateBrightness();
        updateOrientation();
    }

    every(10000) {
        drawWindRose();
    }
}