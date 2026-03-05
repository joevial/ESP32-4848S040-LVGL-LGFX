#include <Arduino.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <TAMC_GT911.h>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include "ui.h"
#include "screens.h"

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

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  
  // Debug: check first pixel
  uint16_t *pixels = (uint16_t *)px_map;
  uint16_t first_pixel = pixels[0];
  
  Serial.printf("Flush: x1=%d y1=%d w=%d h=%d first_pixel=0x%04x\n", area->x1, area->y1, w, h, first_pixel);
  
  lcd.pushImageDMA(area->x1, area->y1, w, h, pixels);
  
  lv_disp_flush_ready(disp);
}

// ==================== Touch Configuration ====================
#define TOUCH_GT911_SCL 45
#define TOUCH_GT911_SDA 19
#define TOUCH_GT911_INT -1
#define TOUCH_GT911_RST -1

static TAMC_GT911 ts(TOUCH_GT911_SDA, TOUCH_GT911_SCL, TOUCH_GT911_INT, TOUCH_GT911_RST, 480, 480);

static int32_t touch_x = 0;
static int32_t touch_y = 0;

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  ts.read();
  
  if (ts.isTouched) {
    // Get touch coordinates
    touch_x = ts.points[0].x;
    touch_y = ts.points[0].y;
    
    data->point.x = touch_x;
    data->point.y = touch_y;
    data->state = LV_INDEV_STATE_PRESSED;
    
    // Update label with coordinates
    static char buffer[32];
    snprintf(buffer, sizeof(buffer), "X:%d Y:%d", touch_x, touch_y);
    lv_label_set_text(objects.label1, buffer);
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);  // Give serial time to initialize
  Serial.println("\n\nStarting LVGL Display with LovyanGFX and GT911 Touch");
  
  // Initialize display
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
  lv_indev_set_read_cb(indev, my_touchpad_read);
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
  
  // Get the active screen (created by lv_display_create)
  lv_obj_t *scr = lv_screen_active();
  extern objects_t objects;
  
  Serial.print("Active screen: ");
  Serial.println((uint32_t)scr);
  Serial.printf("EEZ objects.main: 0x%08x\n", (uint32_t)objects.main);
  Serial.printf("EEZ objects.label1: 0x%08x\n", (uint32_t)objects.label1);
  Serial.flush();
  
  // Set the active screen's background to match the EEZ design
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xff002f77), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  
  // Re-parent the label1 from objects.main to the active screen
  if (objects.label1) {
    lv_obj_set_parent(objects.label1, scr);
    Serial.println("Re-parented label1 to active screen");
    Serial.flush();
  }
  
  // Force full screen render
  lv_obj_invalidate(scr);
  
  // Run timer handler multiple times to ensure initial render is completed
  for (int i = 0; i < 3; i++) {
    lv_timer_handler();
    delay(50);
  }
  
  Serial.println("Setup complete - Display and touch ready!");
}

void loop()
{
  lv_timer_handler();
  ui_tick();
  
  // DEBUG: Force screen refresh to see if data changes
  static uint32_t last_dbg = 0;
  if (millis() - last_dbg > 2000) {
    last_dbg = millis();
    Serial.println("Forcing screen invalidate...");
    lv_obj_invalidate(lv_screen_active());
  }
  
  delay(5);
}