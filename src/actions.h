#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void action_gotomain(lv_event_t * e);
extern void action_gotoradar(lv_event_t * e);
extern void action_radar_right(lv_event_t * e);
extern void action_radar_up(lv_event_t * e);
extern void action_radar_down(lv_event_t * e);
extern void action_radar_left(lv_event_t * e);
extern void action_radar_righ(lv_event_t * e);
extern void action_radar_in(lv_event_t * e);
extern void action_radar_out(lv_event_t * e);
extern void action_gotomenu(lv_event_t * e);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/