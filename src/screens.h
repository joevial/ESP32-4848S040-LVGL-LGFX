#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_MENU = 2,
    SCREEN_ID_RADAR = 3,
    _SCREEN_ID_LAST = 3
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *menu;
    lv_obj_t *radar;
    lv_obj_t *label_temp;
    lv_obj_t *obj0;
    lv_obj_t *button_menu;
    lv_obj_t *label_avg;
    lv_obj_t *label_gust;
    lv_obj_t *label1_3;
    lv_obj_t *label1_4;
    lv_obj_t *label_time;
    lv_obj_t *label_time_1;
    lv_obj_t *label1_5;
    lv_obj_t *label_avg_1;
    lv_obj_t *button_main;
    lv_obj_t *button_radar;
    lv_obj_t *radar_cont;
    lv_obj_t *obj1;
    lv_obj_t *button_exit;
    lv_obj_t *label_timestamp;
    lv_obj_t *label_coords;
    lv_obj_t *label_psram;
    lv_obj_t *btn_up;
    lv_obj_t *btn_down;
    lv_obj_t *btn_left;
    lv_obj_t *btn_right;
    lv_obj_t *btn_in;
    lv_obj_t *btn_out;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_screen_menu();
void tick_screen_menu();

void create_screen_radar();
void tick_screen_radar();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/