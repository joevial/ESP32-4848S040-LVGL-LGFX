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
extern objects_t objects;  // Declared in screens.c
extern "C" void tick_screen_main();
const char* ssid = "mikesnet";
const char* password = "springchicken";
static bool windRoseDirty = false;
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

void notifyTouch() {
    if (!touchOverride && current_brightness != 255) {
        fadeBrightness(255, 200);   // quick 200ms fade to full on touch
        Serial.println("Touch override: brightness -> 255, auto-brightness paused 10s");
    }
    touchOverride      = true;
    touchOverrideStart = millis();
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


#define WIND_DIRECTIONS 16
#define WIND_SPEED_BINS 5
#define WIND_ROSE_CAPACITY 300           // Reduced to 300 points (~100 averaged points) to prevent watchdog timeout during drawing
#define ACCUMULATION_BUFFER_SIZE 20     // Accumulate 20 readings before averaging
#define every(interval) \
    static uint32_t __every__##interval = millis(); \
    if (millis() - __every__##interval >= interval && (__every__##interval = millis()))


struct WindDataPoint {
    float speed;
    float gust;
    float direction;
    unsigned long timestamp;
};

// Wind rose data uses a simple 2D matrix: [direction][speedBin]
// This is the most efficient storage for quick lookup during drawing
int tempHistoryCapacity = 0;
int windHistoryCapacity = 1440;
int *windRoseData[WIND_DIRECTIONS];  // 2D matrix: [direction (0-15)][speedBin (0-4)]
WindDataPoint *windHistory = NULL;
int windHistoryIndex = 0;
int windHistoryCount = 0;
bool windRoseUseGust = false;  // If true, use gust for wind rose; otherwise use average speed
int   windRoseSpeedBins[5] = {5, 10, 15, 20, 0};  // Current speed bins (4 thresholds + padding)

// Accumulate readings before adding to wind rose
WindDataPoint accumulationBuffer[ACCUMULATION_BUFFER_SIZE];
int accumulationIndex = 0;

// Calculate circular mean of direction angles
// Input: array of directions in degrees (0-360)
// Returns: mean direction in degrees (0-360)
float circularMeanDirection(float directions[], int count) {
    if (count == 0) return 0.0f;
    
    float sinSum = 0.0f, cosSum = 0.0f;
    for (int i = 0; i < count; i++) {
        float radians = directions[i] * M_PI / 180.0f;
        sinSum += sinf(radians);
        cosSum += cosf(radians);
    }
    
    float meanRadians = atan2f(sinSum, cosSum);
    float meanDegrees = meanRadians * 180.0f / M_PI;
    
    // Normalize to 0-360
    if (meanDegrees < 0.0f) meanDegrees += 360.0f;
    
    return meanDegrees;
}

// Update addWindData function signature and implementation


void addWindData(float speed, float direction, float gust) {
    if (windHistory == nullptr) return;
    if (!isSetNtp) return;

    float adjSpeed = speed;
    float adjGust  = gust;

    unsigned long now_ts = (unsigned long)time(nullptr);  // ← call ONCE

    windHistory[windHistoryIndex].speed     = adjSpeed;
    windHistory[windHistoryIndex].gust      = adjGust;
    windHistory[windHistoryIndex].direction = direction;
    windHistory[windHistoryIndex].timestamp = now_ts;     // ← reuse

    windHistoryIndex = (windHistoryIndex + 1) % windHistoryCapacity;
    if (windHistoryCount < windHistoryCapacity) windHistoryCount++;

    accumulationBuffer[accumulationIndex].speed     = adjSpeed;
    accumulationBuffer[accumulationIndex].gust      = adjGust;
    accumulationBuffer[accumulationIndex].direction = direction;
    accumulationBuffer[accumulationIndex].timestamp = now_ts;  // ← reuse
    accumulationIndex++;
    // ... rest unchanged

    // When we have 20 readings, calculate circular average and add to wind rose
    if (accumulationIndex >= ACCUMULATION_BUFFER_SIZE) {
        // Calculate averages
        float totalSpeed = 0.0f, totalGust = 0.0f;
        float directions[ACCUMULATION_BUFFER_SIZE];
        
        for (int i = 0; i < ACCUMULATION_BUFFER_SIZE; i++) {
            totalSpeed += accumulationBuffer[i].speed;
            totalGust += accumulationBuffer[i].gust;
            directions[i] = accumulationBuffer[i].direction;
        }
        
        float avgSpeed = totalSpeed / ACCUMULATION_BUFFER_SIZE;
        float avgGust = totalGust / ACCUMULATION_BUFFER_SIZE;
        float meanDir = circularMeanDirection(directions, ACCUMULATION_BUFFER_SIZE);
        
        // Pre-compute direction and speed bins to avoid recalculation during drawing
        int dirBin = (int)((meanDir + 11.25f) / 22.5f) % WIND_DIRECTIONS;
        int spdBin;
        float speedToUse = (windRoseUseGust) ? avgGust : avgSpeed;
        if (speedToUse < windRoseSpeedBins[0]) {
            spdBin = 0;
        } else if (speedToUse < windRoseSpeedBins[1]) {
            spdBin = 1;
        } else if (speedToUse < windRoseSpeedBins[2]) {
            spdBin = 2;
        } else if (speedToUse < windRoseSpeedBins[3]) {
            spdBin = 3;
        } else {
            spdBin = 4;
        }
        
        // Add averaged point to wind rose buffer
        // Increment the matrix directly
        windRoseData[dirBin][spdBin]++;
        
        accumulationIndex = 0;
    }
    windRoseDirty = true;
}


void drawWindRose() {
    // Check if any data exists in the matrix
    int totalCount = 0;
    for (int dir = 0; dir < WIND_DIRECTIONS; dir++) {
        for (int spd = 0; spd < WIND_SPEED_BINS; spd++) {
            totalCount += windRoseData[dir][spd];
        }
    }
    if (totalCount == 0) {
        return;
    }
    
    // Only redraw every 120 seconds to avoid watchdog timeout and reduce overhead
    static uint32_t last_draw_ms = 0;
    uint32_t now = millis();
    if (now - last_draw_ms < 120000) return;  // Skip if less than 120s since last draw
    last_draw_ms = now;
    
    static lv_obj_t * wind_rose_canvas = NULL;
    static lv_draw_buf_t draw_buf;
    static void * canvas_buf = NULL;
    static bool buf_initialized = false;
    
    const int16_t canvasWidth = 275;
    const int16_t canvasHeight = 275;
    const int16_t centerX = canvasWidth / 2;
    const int16_t centerY = canvasHeight / 2;
    const int16_t maxRadius = 137;
    int WINDROSECENTER_X = -85;
    int WINDROSECENTER_Y = 39;
    int DRAW_CENTER_X = 240 + WINDROSECENTER_X;
    int DRAW_CENTER_Y = 240 + WINDROSECENTER_Y;
    lv_color_t speedColors[WIND_SPEED_BINS] = {
        lv_color_hex(0x00FFFF),
        lv_color_hex(0x00FF00),
        lv_color_hex(0xFFFF00),
        lv_color_hex(0xFF8800),
        lv_color_hex(0xFF0000)
    };
    
    if (wind_rose_canvas == NULL) {
        wind_rose_canvas = lv_canvas_create(objects.main);
        
        uint32_t buf_size = canvasWidth * canvasHeight * 4;
        // Free old buffer if it exists (safety check for reallocation)
        if (canvas_buf != NULL) {
            free(canvas_buf);
            canvas_buf = NULL;
        }
        
        canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (canvas_buf == NULL) {
            canvas_buf = malloc(buf_size);
        }
        
        if (canvas_buf != NULL) {
            lv_draw_buf_init(&draw_buf, canvasWidth, canvasHeight, LV_COLOR_FORMAT_ARGB8888, 0, canvas_buf, buf_size);
            lv_canvas_set_draw_buf(wind_rose_canvas, &draw_buf);
            lv_obj_set_pos(wind_rose_canvas, (DRAW_CENTER_X + 3) - centerX, DRAW_CENTER_Y - centerY);
            lv_obj_set_style_bg_opa(wind_rose_canvas, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(wind_rose_canvas, 0, 0);
            lv_obj_set_style_pad_all(wind_rose_canvas, 0, 0);
            buf_initialized = true;
        } else {
            Serial.println("WARNING: Failed to allocate canvas buffer - low memory");
            return;
        }
    }
    
    if (!buf_initialized) return;
    
    lv_canvas_fill_bg(wind_rose_canvas, lv_color_black(), LV_OPA_TRANSP);
    
    // Calculate direction totals from the matrix
    int directionTotals[WIND_DIRECTIONS] = {0};
    int maxDirectionCount = 0;
    
    for (int dir = 0; dir < WIND_DIRECTIONS; dir++) {
        for (int spd = 0; spd < WIND_SPEED_BINS; spd++) {
            directionTotals[dir] += windRoseData[dir][spd];
        }
        if (directionTotals[dir] > maxDirectionCount) {
            maxDirectionCount = directionTotals[dir];
        }
    }
    
    if (maxDirectionCount == 0) return;
    
    lv_layer_t layer;
    lv_canvas_init_layer(wind_rose_canvas, &layer);
    
    // Draw each direction bin (speed bins already pre-computed)
    for (int dir = 0; dir < WIND_DIRECTIONS; dir++) {
        if (directionTotals[dir] == 0) continue;
        
        float spokeLength = (float)directionTotals[dir] / (float)maxDirectionCount * maxRadius;
        float dirAngle = (dir * (360.0f / WIND_DIRECTIONS)) - 90.0f;
        float currentRadius = 0;
        
        // Draw speed bands for this direction
        for (int spd = 0; spd < WIND_SPEED_BINS; spd++) {
            int count = windRoseData[dir][spd];
            if (count == 0) continue;
            
            float bandLength = (float)count / (float)directionTotals[dir] * spokeLength;
            float innerRadius = currentRadius;
            float outerRadius = currentRadius + bandLength;
            
            int16_t startAngle = (int16_t)(dirAngle - (360.0f / WIND_DIRECTIONS) / 2.0f);
            int16_t endAngle = (int16_t)(dirAngle + (360.0f / WIND_DIRECTIONS) / 2.0f);
            
            while (startAngle < 0) startAngle += 360;
            while (endAngle < 0) endAngle += 360;
            
            int numArcs = (int)(bandLength / 3) + 1;  // Reduced from bandLength/2 for fewer arcs
            if (numArcs < 1) numArcs = 1;
            
            for (int a = 0; a < numArcs; a++) {
                float radius = innerRadius + (bandLength * a / numArcs) + (bandLength / numArcs / 2);
                int arcWidth = (int)(bandLength / numArcs) + 2;  // Increased width by 1 to compensate for fewer draws
                if (arcWidth < 2) arcWidth = 2;
                
                lv_draw_arc_dsc_t arc_dsc;
                lv_draw_arc_dsc_init(&arc_dsc);
                arc_dsc.color = speedColors[spd];
                arc_dsc.width = arcWidth;
                arc_dsc.opa = LV_OPA_COVER;
                arc_dsc.rounded = 0;
                arc_dsc.center.x = centerX;
                arc_dsc.center.y = centerY;
                arc_dsc.start_angle = startAngle;
                arc_dsc.end_angle = endAngle;
                arc_dsc.radius = (int16_t)radius;
                
                lv_draw_arc(&layer, &arc_dsc);
            }
            
            currentRadius = outerRadius;
        }
    }
    
    lv_canvas_finish_layer(wind_rose_canvas, &layer);
}

// ── updateWindTimestamp: Update label_time_1 with time span of wind rose data ──
// Shows the total time span from oldest to newest wind history entry
// Formats as: "Xs" for <60s, "Xm" for <60m, "Xh" for >=60m
void updateWindTimestamp() {
    if (windHistoryCount == 0) {
        lv_label_set_text(objects.label_time_1, "--");
        return;
    }

    // Circular buffer: oldest entry is at windHistoryIndex when full,
    // or at 0 when not yet wrapped. Newest is always one behind windHistoryIndex.
    int oldestIdx = (windHistoryCount < windHistoryCapacity) ? 0 
                    : windHistoryIndex;
    int newestIdx = (windHistoryIndex - 1 + windHistoryCapacity) % windHistoryCapacity;

    unsigned long oldestTimestamp = windHistory[oldestIdx].timestamp;
    unsigned long newestTimestamp = windHistory[newestIdx].timestamp;

    if (oldestTimestamp == 0 || newestTimestamp == 0 || oldestTimestamp == newestTimestamp) {
        lv_label_set_text(objects.label_time_1, "--");
        return;
    }

    unsigned long spanSecs = newestTimestamp - oldestTimestamp;

    if (spanSecs < 60) {
        lv_label_set_text_fmt(objects.label_time_1, "%lus", spanSecs);
    } else if (spanSecs < 3600) {
        lv_label_set_text_fmt(objects.label_time_1, "%lum", spanSecs / 60);
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

  // Allocate wind history
  windHistory = (WindDataPoint *)malloc(windHistoryCapacity * sizeof(WindDataPoint));
  if (windHistory == NULL) {
    Serial.println("ERROR: Could not allocate wind history!");
    while(1) delay(1000);
  }
  
  // Allocate wind rose 2D matrix (16 directions × 5 speed bins = 320 bytes)
  for (int i = 0; i < WIND_DIRECTIONS; i++) {
    windRoseData[i] = (int *)malloc(WIND_SPEED_BINS * sizeof(int));
    if (windRoseData[i] == NULL) {
      Serial.println("ERROR: Could not allocate wind rose matrix!");
      while(1) delay(1000);
    }
    memset(windRoseData[i], 0, WIND_SPEED_BINS * sizeof(int));
  }
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