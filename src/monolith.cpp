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
#include <BlynkSimpleEsp32.h>
#include <WiFi.h>
#include "time.h"

extern objects_t objects;  // Declared in screens.c
extern "C" void tick_screen_main();
char auth[] = "8_-CN2rm4ki9P3i_NkPhxIbCiKd5RXhK";  //hubert
const char* ssid = "mikesnet";
const char* password = "springchicken";

float windavg, windgust, winddir, temp;
int hours, mins, secs;
unsigned long localTimeUnix = 0; 
unsigned long s1LastUpdate = 0;
struct tm timeinfo;
bool isSetNtp = false;

// ==================== Sensor I2C Bus (GPIO1=SDA, GPIO40=SCL) ====================
// Touch uses Wire (I2C_NUM_1, SDA=19, SCL=45) — sensors get their own TwoWire instance
TwoWire sensorI2C = TwoWire(0);   // I2C_NUM_0

// ==================== Backlight PWM (manual LEDC, bypasses LovyanGFX) ====================
#define GFX_BL              GPIO_NUM_38
#define BL_PWM_FREQ         100000    // 100 kHz — above audible range, no coil whine
#define BL_PWM_RESOLUTION   8
#define BL_MAX              255
#define BL_MIN              60            // ~20% duty — below this LEDs don't conduct

// BH1750 — ambient light sensor for automatic backlight control
BH1750FVI myLux(0x23, &sensorI2C);
static float    bh1750_lux        = -1.0f;
static uint8_t  current_brightness = BL_MAX;



// Touch brightness override — disables auto-brightness for 10 s after any touch
#define TOUCH_BRIGHTNESS        255
#define TOUCH_OVERRIDE_MS       10000UL
static bool     touchOverride      = false;
static uint32_t touchOverrideStart = 0;

void notifyTouch() {
    if (!touchOverride && current_brightness != TOUCH_BRIGHTNESS) {
        ledcWrite(GFX_BL, TOUCH_BRIGHTNESS);
        current_brightness = TOUCH_BRIGHTNESS;
        Serial.println("Touch override: brightness -> 128, auto-brightness paused 10s");
    }
    touchOverride      = true;
    touchOverrideStart = millis();  // reset the 10 s window on every touch
}


// MPU-9250 — 9-axis IMU for display orientation detection
MPU9250_WE mpu(&sensorI2C, 0x68);
static uint8_t current_rotation    = 0;       // 0/1/2/3 → 0°/90°/180°/270°

// Forward declarations
void initSensors();
void updateBrightness();
void updateOrientation();
static uint8_t luxToBrightness(float lux);
void addWindData(float speed, float direction, float gust);
void drawWindRose();
void cbSyncTime(struct timeval *tv);
void initSNTP();
void setTimezone();
void applyBootOrientation();

void updateBrightness() {
    // Expire touch override after 10 s, then resume auto-brightness
    if (touchOverride && (millis() - touchOverrideStart >= TOUCH_OVERRIDE_MS)) {
        touchOverride = false;
        Serial.println("Touch override expired, resuming auto-brightness");
    }
    if (touchOverride) return;

    float lux = myLux.getLux();
    Serial.printf("BH1750 raw lux: %.1f\n", lux);
    if (lux < 0) return;
    bh1750_lux = lux;

    uint8_t target = luxToBrightness(lux);
    if (abs((int)target - (int)current_brightness) > 3) {
        ledcWrite(GFX_BL, target);
        Serial.printf("BH1750 %.1f lux → brightness %d → %d\n", lux, current_brightness, target);
        current_brightness = target;
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
#define every(interval) \
    static uint32_t __every__##interval = millis(); \
    if (millis() - __every__##interval >= interval && (__every__##interval = millis()))


struct WindDataPoint {
    float speed;
    float gust;      // ADD THIS
    float direction;
    unsigned long timestamp;
};
int tempHistoryCapacity = 0;
int windHistoryCapacity = 1440;
int *windRoseData[WIND_DIRECTIONS];
WindDataPoint *windHistory = NULL;
int windHistoryIndex = 0;
int windHistoryCount = 0;
bool windRoseUseGust = false;  // If true, use gust for wind rose; otherwise use average speed
int   windRoseSpeedBins[5] = {5, 10, 20, 30, 0};  // Current speed bins (4 thresholds + padding)
// Update addWindData function signature and implementation


void addWindData(float speed, float direction, float gust) {
    if (windHistory == nullptr) return;

    // Apply wind offset multiplier
    float adjSpeed = speed;
    float adjGust  = gust;

    windHistory[windHistoryIndex].speed     = adjSpeed;
    windHistory[windHistoryIndex].gust      = adjGust;
    windHistory[windHistoryIndex].direction = direction;
    windHistory[windHistoryIndex].timestamp = (unsigned long)time(nullptr);

    windHistoryIndex = (windHistoryIndex + 1) % windHistoryCapacity;
    if (windHistoryCount < windHistoryCapacity) windHistoryCount++;

    // Rebuild wind rose from history
    for (int i = 0; i < WIND_DIRECTIONS; i++) {
        if (windRoseData[i] != nullptr)
            for (int j = 0; j < WIND_SPEED_BINS; j++)
                windRoseData[i][j] = 0;
    }

    int startIdx = (windHistoryIndex - windHistoryCount + windHistoryCapacity) % windHistoryCapacity;
    for (int i = 0; i < windHistoryCount; i++) {
        int idx   = (startIdx + i) % windHistoryCapacity;
        
        // Choose data source: gust if Switch3 is checked, otherwise speed
        float spd = (windRoseUseGust) ? windHistory[idx].gust : windHistory[idx].speed;
        float dir = windHistory[idx].direction;
        int dirBin = (int)((dir + 11.25f) / 22.5f) % WIND_DIRECTIONS;
        
        // Calculate speed bin based on configured thresholds
        int spdBin;
        if (spd < windRoseSpeedBins[0]) {
            spdBin = 0;
        } else if (spd < windRoseSpeedBins[1]) {
            spdBin = 1;
        } else if (spd < windRoseSpeedBins[2]) {
            spdBin = 2;
        } else if (spd < windRoseSpeedBins[3]) {
            spdBin = 3;
        } else {
            spdBin = 4;
        }
        
        if (windRoseData[dirBin] != nullptr)
            windRoseData[dirBin][spdBin]++;
    }
}


void drawWindRose() {
    if (windHistory == nullptr || windHistoryCount == 0) return;
    
    static lv_obj_t * wind_rose_canvas = NULL;
    static lv_draw_buf_t draw_buf;
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
        void * buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            buf = malloc(buf_size);
        }
        
        if (buf != NULL) {
            lv_draw_buf_init(&draw_buf, canvasWidth, canvasHeight, LV_COLOR_FORMAT_ARGB8888, 0, buf, buf_size);
            lv_canvas_set_draw_buf(wind_rose_canvas, &draw_buf);
            lv_obj_set_pos(wind_rose_canvas, (DRAW_CENTER_X + 3) - centerX, DRAW_CENTER_Y - centerY);
            lv_obj_set_style_bg_opa(wind_rose_canvas, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(wind_rose_canvas, 0, 0);
            lv_obj_set_style_pad_all(wind_rose_canvas, 0, 0);
            buf_initialized = true;
        } else {
            //Serial.println("Failed to allocate canvas buffer");
            return;
        }
    }
    
    if (!buf_initialized) return;
    
    lv_canvas_fill_bg(wind_rose_canvas, lv_color_black(), LV_OPA_TRANSP);
    
    int directionTotals[WIND_DIRECTIONS] = {0};
    int maxDirectionCount = 0;
    
    for (int dir = 0; dir < WIND_DIRECTIONS; dir++) {
        if (windRoseData[dir] != nullptr) {
            for (int spd = 0; spd < WIND_SPEED_BINS; spd++) {
                directionTotals[dir] += windRoseData[dir][spd];
            }
            if (directionTotals[dir] > maxDirectionCount) {
                maxDirectionCount = directionTotals[dir];
            }
        }
    }
    
    if (maxDirectionCount == 0) return;
    
    lv_layer_t layer;
    lv_canvas_init_layer(wind_rose_canvas, &layer);
    
    for (int dir = 0; dir < WIND_DIRECTIONS; dir++) {
        if (windRoseData[dir] == nullptr || directionTotals[dir] == 0) continue;
        
        float spokeLength = (float)directionTotals[dir] / (float)maxDirectionCount * maxRadius;
        float dirAngle = (dir * 22.5f) - 90.0f;
        float currentRadius = 0;
        
        for (int spd = 0; spd < WIND_SPEED_BINS; spd++) {
            int count = windRoseData[dir][spd];
            if (count == 0) continue;
            
            float bandLength = (float)count / (float)directionTotals[dir] * spokeLength;
            float innerRadius = currentRadius;
            float outerRadius = currentRadius + bandLength;
            
            int16_t startAngle = (int16_t)(dirAngle - 11.25f);
            int16_t endAngle = (int16_t)(dirAngle + 11.25f);
            
            while (startAngle < 0) startAngle += 360;
            while (endAngle < 0) endAngle += 360;
            
            int numArcs = (int)(bandLength / 2) + 1;
            if (numArcs < 1) numArcs = 1;
            
            for (int a = 0; a < numArcs; a++) {
                float radius = innerRadius + (bandLength * a / numArcs) + (bandLength / numArcs / 2);
                int arcWidth = (int)(bandLength / numArcs) + 1;
                if (arcWidth < 1) arcWidth = 1;
                
                lv_draw_arc_dsc_t arc_dsc;
                lv_draw_arc_dsc_init(&arc_dsc);
                arc_dsc.color = speedColors[spd];
                arc_dsc.width = arcWidth;
                arc_dsc.opa = LV_OPA_COVER;
                arc_dsc.rounded = 0;
                arc_dsc.center.x = centerX;
                arc_dsc.center.y = centerY;
                arc_dsc.radius = (int16_t)radius;
                arc_dsc.start_angle = startAngle;
                arc_dsc.end_angle = endAngle;
                
                lv_draw_arc(&layer, &arc_dsc);
            }
            
            currentRadius = outerRadius;
        }
    }
    
    lv_canvas_finish_layer(wind_rose_canvas, &layer);
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V84, V85, V86, V118);
}


BLYNK_WRITE(V84) {
  windavg = param.asFloat();
  lv_label_set_text_fmt(objects.label_avg, "%.1fkph", windavg);
}

BLYNK_WRITE(V85) {
  windgust = param.asFloat();
  lv_label_set_text_fmt(objects.label_gust, "%.1fkph", windgust);
}

BLYNK_WRITE(V86) {
  winddir = param.asFloat();
  addWindData(windavg, winddir, windgust);
}

BLYNK_WRITE(V118) {
  temp = param.asFloat();
  lv_label_set_text_fmt(objects.label_temp, "%.1f°C", temp);
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
      cfg.freq_write  = 14000000;

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

    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = GPIO_NUM_38;
      _light_instance.config(cfg);
    }
    _panel_instance.light(&_light_instance);

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

     _panel_instance.light(&_light_instance);

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
    lcd.waitDMA();  // Wait for previous DMA to finish
    lcd.pushImageDMA(area->x1, area->y1, w, h, (uint16_t *)px_map);
    lv_disp_flush_ready(disp);  // Safe to call after waitDMA
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

// Map lux → 8-bit backlight PWM (BL_MIN–255).
static uint8_t luxToBrightness(float lux) {
    if (lux <= 0.0f)  return BL_MIN;
    if (lux >= 5.0f)  return BL_MAX;
    // Logarithmic mapping over 0.0 – 5.0 lux
    float norm = log10f(lux + 1.0f) / log10f(6.0f);  // 0.0 – 1.0
    float b    = BL_MIN + norm * (BL_MAX - BL_MIN);
    return (uint8_t)constrain((int)b, BL_MIN, BL_MAX);
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

    //if (new_rotation != 0xFF)
   //     Serial.printf("MPU-9250 orientation: %s -> rotation %d deg\n", orientationName(orient), new_rotation * 90);
   // else
  //      Serial.printf("MPU-9250 orientation: %s (flat/ambiguous, ignored)\n", orientationName(orient));

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
  //delay(1000);  // Give serial time to initialize
  Serial.println("\n\nStarting LVGL Display with LovyanGFX and GT911 Touch");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); //low power for better connectivity


  // Allocate wind history
  windHistory = (WindDataPoint *)malloc(windHistoryCapacity * sizeof(WindDataPoint));
  if (windHistory == NULL) {
    Serial.println("ERROR: Could not allocate wind history!");
    while(1) delay(1000);
  }
  
  // Allocate wind rose data
  for (int i = 0; i < WIND_DIRECTIONS; i++) {
    windRoseData[i] = (int *)malloc(WIND_SPEED_BINS * sizeof(int));
    if (windRoseData[i] == NULL) {
      Serial.println("ERROR: Could not allocate wind rose data!");
      while(1) delay(1000);
    }
    memset(windRoseData[i], 0, WIND_SPEED_BINS * sizeof(int));
  }
  
  Serial.println("Initializing display...");

  lcd.init();
  delay(50);


  
  Serial.println("Display initialized");

  initSensors();
  applyBootOrientation();

  lv_init();
  
  static lv_display_t *disp = lv_display_create(screenWidth, screenHeight);

  
  
  // Allocate draw buffer from PSRAM if available
  static uint8_t *disp_draw_buf = NULL;
  
  // Try PSRAM first (SPIRAM)
  disp_draw_buf = (uint8_t *)heap_caps_malloc(screenWidth * 60 * 2, MALLOC_CAP_SPIRAM);
  if (disp_draw_buf) {
    Serial.println("Allocated display buffer from PSRAM");
  } else {
    // Fallback to DRAM if PSRAM unavailable (not recommended)
    disp_draw_buf = (uint8_t *)heap_caps_malloc(screenWidth * 60 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.println("Allocated display buffer from SRAM (PSRAM unavailable)");
  }
  
  if (!disp_draw_buf) {
    Serial.println("ERROR: Could not allocate display buffer!");
    while (1) delay(1000);
  }

  
  
  // Try PARTIAL mode first to avoid potential hang
  lv_display_set_buffers(disp, disp_draw_buf, NULL, screenWidth * 60 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_resolution(disp, screenWidth, screenHeight);
  lv_display_set_default(disp);

  
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read);

  ui_init();
  Serial.println("UI initialized");

  initSNTP();
  Serial.println("Setup complete - Display and touch ready!");
  ledcAttach(GFX_BL, BL_PWM_FREQ, BL_PWM_RESOLUTION);
  ledcDetach(GFX_BL);
  ledcAttach(GFX_BL, BL_PWM_FREQ, BL_PWM_RESOLUTION);
  ledcWrite(GFX_BL, BL_MAX);
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
            Blynk.config(auth, IPAddress(192, 168, 50, 197), 8080);
            Blynk.connect();
            ArduinoOTA.setHostname("monolith");
            ArduinoOTA.begin();
            connected = true;
        }
        else {
            Blynk.run();
            ArduinoOTA.handle();
        }
    }
    every(5){
    lv_timer_handler();
    ui_tick();
    lv_refr_now(NULL);
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
    // ---------------------------------------------------------------------

    // Auto-brightness from BH1750
    every(500) {
        updateBrightness();
        updateOrientation();
    }


    every(10000){
    Blynk.syncVirtual(V84);
    Blynk.syncVirtual(V85);
    Blynk.syncVirtual(V86);
    Blynk.syncVirtual(V118);
        drawWindRose();
    }
    
}