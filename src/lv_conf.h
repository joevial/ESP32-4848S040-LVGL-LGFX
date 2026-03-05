/**
 * @file lv_conf.h
 * Configuration file for v9.4
 */

/*
 * Copy this file as `lv_conf.h`
 * 1. simply next to the `lvgl` folder
 * 2. or any other places and
 *    - define `LV_CONF_INCLUDE_SIMPLE`
 *    - add the path as include path
 */

/* clang-format off */
#if 1 /*Set it to "1" to enable content*/

#ifndef LV_CONF_H
#define LV_CONF_H

/*If you need to include anything here, do it inside the `__ASSEMBLY__` guard */
#if  0 && defined(__ASSEMBLY__)
#include "my_include.h"
#endif

/*====================
   COLOR SETTINGS
 *====================*/

/*Color depth: 1 (I1), 8 (L8), 16 (RGB565), 24 (RGB888), 32 (XRGB8888)*/
#define LV_COLOR_DEPTH 16

/*Swap the 2 bytes of RGB565 color*/
#define LV_COLOR_16_SWAP 1

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/

/* Possible values
 * - LV_STDLIB_BUILTIN:     LVGL's built in implementation
 * - LV_STDLIB_CLIB:        Standard C functions, like malloc, strlen, etc
 * - LV_STDLIB_MICROPYTHON: MicroPython implementation
 * - LV_STDLIB_RTTHREAD:    RT-Thread implementation
 * - LV_STDLIB_CUSTOM:      Implement the functions externally
 */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_LIMITS_INCLUDE       <limits.h>
#define LV_STDARG_INCLUDE       <stdarg.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    /*Size of the memory available for `lv_malloc()` in bytes (>= 2kB)*/
    #define LV_MEM_SIZE (192 * 1024U)

    /*Size of the memory expand for `lv_malloc()` in bytes*/
    #define LV_MEM_POOL_EXPAND_SIZE 0

    /*Set an address for the memory pool instead of allocating it as a normal array. Can be in external SRAM too.*/
    #define LV_MEM_ADR 0
    /*Instead of an address give a memory allocator that will be called to get a memory pool for LVGL. E.g. my_malloc*/
    #if LV_MEM_ADR == 0
        #undef LV_MEM_POOL_INCLUDE
        #undef LV_MEM_POOL_ALLOC
    #endif
#endif  /*LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN*/

/*====================
   HAL SETTINGS
 *====================*/

/*Default display refresh, input device read and animation step period.*/
#define LV_DEF_REFR_PERIOD  20

/*Default Dot Per Inch. Used to initialize default sizes such as widgets sized, style paddings.
 *(Not so important, you can adjust it to modify default sizes and spaces)*/
#define LV_DPI_DEF 130

/*Custom tick source for Arduino*/
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

/*=================
 * OPERATING SYSTEM
 *=================*/
/*Select an operating system to use. Possible options:
 * - LV_OS_NONE
 * - LV_OS_PTHREAD
 * - LV_OS_FREERTOS
 * - LV_OS_CMSIS_RTOS2
 * - LV_OS_RTTHREAD
 * - LV_OS_WINDOWS
 * - LV_OS_MQX
 * - LV_OS_CUSTOM */
#define LV_USE_OS   LV_OS_FREERTOS

#if LV_USE_OS == LV_OS_CUSTOM
    #define LV_OS_CUSTOM_INCLUDE <stdint.h>
#endif
#if LV_USE_OS == LV_OS_FREERTOS
	/*
	 * Unblocking an RTOS task with a direct notification is 45% faster and uses less RAM
	 * than unblocking a task using an intermediary object such as a binary semaphore.
	 * RTOS task notifications can only be used when there is only one task that can be the recipient of the event.
	 */
	#define LV_USE_FREERTOS_TASK_NOTIFY 0
#endif

/*========================
 * RENDERING CONFIGURATION
 *========================*/

/*Align the stride of all layers and images to this bytes*/
#define LV_DRAW_BUF_STRIDE_ALIGN                1

/*Align the start address of draw_buf addresses to this bytes*/
#define LV_DRAW_BUF_ALIGN                       4

/*Using matrix for transformations*/
#define LV_DRAW_TRANSFORM_USE_MATRIX            0

/*The target buffer size for simple layer chunks.*/
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE    (24 * 1024)

/* The stack size of the drawing thread */
#define LV_DRAW_THREAD_STACK_SIZE    (8 * 1024)

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1

	#define LV_DRAW_SW_SUPPORT_RGB565		1
	#define LV_DRAW_SW_SUPPORT_RGB565A8		1
	#define LV_DRAW_SW_SUPPORT_RGB888		1
	#define LV_DRAW_SW_SUPPORT_XRGB8888		1
	#define LV_DRAW_SW_SUPPORT_ARGB8888		1
	#define LV_DRAW_SW_SUPPORT_L8			1
	#define LV_DRAW_SW_SUPPORT_AL88			1
	#define LV_DRAW_SW_SUPPORT_A8			1
	#define LV_DRAW_SW_SUPPORT_I1			1

    #define LV_DRAW_SW_DRAW_UNIT_CNT    1

    #define LV_USE_DRAW_ARM2D_SYNC      0

    #define LV_USE_NATIVE_HELIUM_ASM    0

    #define LV_DRAW_SW_COMPLEX          1

    #if LV_DRAW_SW_COMPLEX == 1
        #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
        #define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
    #endif

    #define  LV_USE_DRAW_SW_ASM     LV_DRAW_SW_ASM_NONE

    #if LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM
        #define  LV_DRAW_SW_ASM_CUSTOM_INCLUDE ""
    #endif

    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS    0
#endif

#define LV_USE_DRAW_VGLITE 0
#define LV_USE_PXP 0
#define LV_USE_DRAW_DAVE2D 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_DRAW_VG_LITE 0

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

/*Enable the log module*/
#define LV_USE_LOG 0

/*Enable asserts if an operation is failed or an invalid data is found*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while(1);

/*1: Draw random colored rectangles over the redrawn areas*/
#define LV_USE_REFR_DEBUG 0

/*1: Draw a red overlay for ARGB layers and a green overlay for RGB layers*/
#define LV_USE_LAYER_DEBUG 0

/*1: Draw overlays with different colors for each draw_unit's tasks*/
#define LV_USE_PARALLEL_DRAW_DEBUG 0

#define LV_ENABLE_GLOBAL_CUSTOM 0

/*Default cache size in bytes*/
#define LV_CACHE_DEF_SIZE       0

/*Default number of image header cache entries*/
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0

/*Number of stops allowed per gradient*/
#define LV_GRADIENT_MAX_STOPS   2

/* Adjust color mix functions rounding */
#define LV_COLOR_MIX_ROUND_OFS  0

/* Add 2 x 32 bit variables to each lv_obj_t to speed up getting style properties */
#define LV_OBJ_STYLE_CACHE      0

/* Add `id` field to `lv_obj_t` */
#define LV_USE_OBJ_ID           0

/* Automatically assign an ID when obj is created */
#define LV_OBJ_ID_AUTO_ASSIGN   LV_USE_OBJ_ID

/*Use the builtin obj ID handler functions*/
#define LV_USE_OBJ_ID_BUILTIN   1

/*Use obj property set/get API*/
#define LV_USE_OBJ_PROPERTY 0

/*Enable property name support*/
#define LV_USE_OBJ_PROPERTY_NAME 1

/*=====================
 *  COMPILER SETTINGS
 *====================*/

/*For big endian systems set to 1*/
#define LV_BIG_ENDIAN_SYSTEM 0

#define LV_ATTRIBUTE_TICK_INC

#define LV_ATTRIBUTE_TIMER_HANDLER

#define LV_ATTRIBUTE_FLUSH_READY

#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1

#define LV_ATTRIBUTE_MEM_ALIGN

#define LV_ATTRIBUTE_LARGE_CONST

#define LV_ATTRIBUTE_LARGE_RAM_ARRAY

#define LV_ATTRIBUTE_FAST_MEM

#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning

#define LV_ATTRIBUTE_EXTERN_DATA

/* Use `float` as `lv_value_precise_t` */
#define LV_USE_FLOAT            0

/*Enable matrix support*/
#define LV_USE_MATRIX           0

/*Include `lvgl_private.h` in `lvgl.h` to access internal data and functions*/
#define LV_USE_PRIVATE_API		0

/*==================
 *   FONT USAGE
 *===================*/

/*Montserrat fonts with ASCII range and some symbols using bpp = 4*/
#define LV_FONT_MONTSERRAT_8  1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_46 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_14_CJK            0
#define LV_FONT_SIMSUN_16_CJK            0

/*Pixel perfect monospace fonts*/
#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0

#define LV_FONT_CUSTOM_DECLARE

#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_FONT_FMT_TXT_LARGE 0

#define LV_USE_FONT_COMPRESSED 0

#define LV_USE_FONT_PLACEHOLDER 1

/*=================
 *  TEXT SETTINGS
 *=================*/

#define LV_TXT_ENC LV_TXT_ENC_UTF8

/*Can break (wrap) texts on these chars*/
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"

#define LV_TXT_LINE_BREAK_LONG_LEN 0

#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN 3

#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3

#define LV_USE_BIDI 0

#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*==================
 * WIDGETS
 *================*/

#define LV_WIDGETS_HAS_DEFAULT_VALUE  1
#define LV_USE_ANIMIMG    1
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BUTTON        1
#define LV_USE_BUTTONMATRIX  1
#define LV_USE_CALENDAR   1
#define LV_USE_CANVAS     1
#define LV_USE_CHART      1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMAGE      1
#define LV_USE_IMAGEBUTTON     1
#define LV_USE_KEYBOARD   1

#define LV_USE_LABEL      1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 1
    #define LV_LABEL_LONG_TXT_HINT 1
    #define LV_LABEL_WAIT_CHAR_COUNT 3
#endif

#define LV_USE_LED        1
#define LV_USE_LINE       1
#define LV_USE_LIST       1
#define LV_USE_LOTTIE     0
#define LV_USE_MENU       1
#define LV_USE_MSGBOX     1
#define LV_USE_ROLLER     1
#define LV_USE_SCALE      1
#define LV_USE_SLIDER     1
#define LV_USE_SPAN       1
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_SWITCH     1

#define LV_USE_TEXTAREA   1
#if LV_USE_TEXTAREA != 0
    #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif

#define LV_USE_TABLE      1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1

/*==================
 * THEMES
 *==================*/

#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 1
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif

#define LV_USE_THEME_SIMPLE 1
#define LV_USE_THEME_MONO 1

/*==================
 * LAYOUTS
 *==================*/

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*====================
 * 3RD PARTS LIBRARIES
 *====================*/

#define LV_FS_DEFAULT_DRIVER_LETTER '\0'
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_MEMFS 0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_FS_ARDUINO_ESP_LITTLEFS 0
#define LV_USE_FS_ARDUINO_SD 0

#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF 0
#define LV_BIN_DECODER_RAM_LOAD 0
#define LV_USE_RLE 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_RLOTTIE 0
#define LV_USE_VECTOR_GRAPHIC  0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_USE_LZ4_INTERNAL  0
#define LV_USE_LZ4_EXTERNAL  0
#define LV_USE_FFMPEG 0

/*==================
 * OTHERS
 *==================*/

#define LV_USE_SNAPSHOT 0
#define LV_USE_SYSMON   0
#define LV_USE_PROFILER 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_OBSERVER 1
#define LV_USE_IME_PINYIN 0
#define LV_USE_FILE_EXPLORER                     0

/*==================
 * DEVICES
 *==================*/

#define LV_USE_SDL              0
#define LV_USE_X11              0
#define LV_USE_WAYLAND          0

#endif /*LV_CONF_H*/

#endif /*End of "Content enable"*/
