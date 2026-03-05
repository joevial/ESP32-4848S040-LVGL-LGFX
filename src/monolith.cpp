#include <Arduino.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <TAMC_GT911.h>
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
const char* ssid = "xxx";
const char* password = "xxx";

float windavg, windgust, winddir, temp;
int hours, mins, secs;
unsigned long localTimeUnix = 0; 
unsigned long s1LastUpdate = 0;
struct tm timeinfo;
bool isSetNtp = false;
void addWindData(float speed, float direction, float gust);
void drawWindRose();
void cbSyncTime(struct timeval *tv);
void initSNTP();
void setTimezone();



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
    static int flushCount = 0;
    flushCount++;
    if (flushCount % 100 == 0) {
        Serial.printf("flush count=%d\n", flushCount);
    }
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    lcd.waitDMA();  // Wait for previous DMA to finish
    lcd.pushImageDMA(area->x1, area->y1, w, h, (uint16_t *)px_map);
    lv_disp_flush_ready(disp);  // Safe to call after waitDMA
}

// ==================== Touch Configuration ====================
#define TOUCH_GT911_SCL 45
#define TOUCH_GT911_SDA 19
#define TOUCH_GT911_INT -1
#define TOUCH_GT911_RST -1

static TAMC_GT911 ts(TOUCH_GT911_SDA, TOUCH_GT911_SCL, TOUCH_GT911_INT, TOUCH_GT911_RST, 480, 480);

static int32_t touch_x = 0;
static int32_t touch_y = 0;



void setup()
{
  Serial.begin(115200);
  //delay(1000);  // Give serial time to initialize
  Serial.println("\n\nStarting LVGL Display with LovyanGFX and GT911 Touch");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
    WiFi.setTxPower(WIFI_POWER_8_5dBm); //low power for better connectivity
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  ArduinoOTA.setHostname("monolith");
    
    
  ArduinoOTA.begin();
  Blynk.config(auth, IPAddress(192, 168, 50, 197), 8080);
  Blynk.connect();
  
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
  lcd.setBrightness(255);
  
  Serial.println("Display initialized");
  
  // Initialize touch
  Serial.println("Initializing touch...");
  Wire.begin(TOUCH_GT911_SDA, TOUCH_GT911_SCL);
  ts.begin();
  ts.setRotation(0);
  
  Serial.println("Touch initialized");
  
  // Initialize LVGL
  Serial.println("Initializing LVGL...");
  Serial.flush();
  lv_init();
  Serial.println("DEBUG: lv_init() complete");
  Serial.flush();
  
  // Create display for LVGL v9.4 API
  Serial.println("DEBUG: About to create display...");
  Serial.flush();
  static lv_display_t *disp = lv_display_create(screenWidth, screenHeight);
  Serial.println("DEBUG: Display created");
  Serial.flush();
  
  // Allocate draw buffer from PSRAM if available
  static uint8_t *disp_draw_buf = NULL;
  
  // Try PSRAM first (SPIRAM)
  disp_draw_buf = (uint8_t *)heap_caps_malloc(screenWidth * 60 * 2, MALLOC_CAP_SPIRAM);
  if (disp_draw_buf) {
    Serial.println("Allocated display buffer from PSRAM");
    Serial.flush();
  } else {
    // Fallback to DRAM if PSRAM unavailable (not recommended)
    disp_draw_buf = (uint8_t *)heap_caps_malloc(screenWidth * 60 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.println("Allocated display buffer from SRAM (PSRAM unavailable)");
    Serial.flush();
  }
  
  if (!disp_draw_buf) {
    Serial.println("ERROR: Could not allocate display buffer!");
    while (1) delay(1000);
  }
  
  Serial.println("DEBUG: About to set buffers...");
  Serial.flush();
  
  // Try PARTIAL mode first to avoid potential hang
  lv_display_set_buffers(disp, disp_draw_buf, NULL, screenWidth * 60 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
  
  Serial.println("DEBUG: Buffers set");
  Serial.flush();
  
  Serial.println("DEBUG: About to set flush callback...");
  Serial.flush();
  lv_display_set_flush_cb(disp, my_disp_flush);
  Serial.println("DEBUG: Flush callback set");
  Serial.flush();
  
  Serial.println("DEBUG: About to set resolution...");
  Serial.flush();
  lv_display_set_resolution(disp, screenWidth, screenHeight);
  Serial.println("DEBUG: Resolution set");
  Serial.flush();
  
  Serial.println("DEBUG: About to set as default...");
  Serial.flush();
  lv_display_set_default(disp);
  Serial.println("DEBUG: Set as default");
  Serial.flush();
  
  Serial.println("LVGL display configured");
  Serial.flush();
  
  // Create input device
  Serial.println("DEBUG: About to create input device...");
  Serial.flush();
  lv_indev_t *indev = lv_indev_create();
  Serial.println("DEBUG: Input device created");
  Serial.flush();
  
  Serial.println("DEBUG: About to set input device type...");
  Serial.flush();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  Serial.println("DEBUG: Input device type set");
  Serial.flush();
  
  Serial.println("DEBUG: About to set input device read callback...");
  Serial.flush();

  Serial.println("DEBUG: Input device read callback set");
  Serial.flush();
  
  Serial.println("LVGL input device configured");
  Serial.flush();
  
  // Create UI
  Serial.println("Creating UI...");
  Serial.flush();
  ui_init();
  Serial.println("UI initialized");
  Serial.flush();
  
  // Force full screen render
  //lv_obj_invalidate(objects.main);
  
  // Run timer handler multiple times to ensure initial render is completed
  //for (int i = 0; i < 3; i++) {
  //  lv_timer_handler();
  //  delay(50);
  //}
  initSNTP();
  Serial.println("Setup complete - Display and touch ready!");
}

void loop() {
    lv_timer_handler();
    ui_tick();
    lv_refr_now(NULL);
    ArduinoOTA.handle();
    Blynk.run();
    
    every(500) {
        drawWindRose();
    }
    every(60000){
    Blynk.syncVirtual(V84);
    Blynk.syncVirtual(V85);
    Blynk.syncVirtual(V86);
    Blynk.syncVirtual(V118);
    }
}