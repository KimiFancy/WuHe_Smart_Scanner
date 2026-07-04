#include "screen_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "esp_lcd_ili9341.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "lvgl.h"


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ     (20 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
// #define EXAMPLE_PIN_NUM_SCLK           18
// #define EXAMPLE_PIN_NUM_MOSI           19
// #define EXAMPLE_PIN_NUM_MISO           21
#define EXAMPLE_PIN_NUM_SCLK           36
#define EXAMPLE_PIN_NUM_MOSI           35
#define EXAMPLE_PIN_NUM_MISO           37
// #define EXAMPLE_PIN_NUM_LCD_DC         5
// #define EXAMPLE_PIN_NUM_LCD_RST        3
// #define EXAMPLE_PIN_NUM_LCD_CS         4
// #define EXAMPLE_PIN_NUM_BK_LIGHT       2
#define EXAMPLE_PIN_NUM_LCD_DC         39
#define EXAMPLE_PIN_NUM_LCD_RST        40
#define EXAMPLE_PIN_NUM_LCD_CS         38
#define EXAMPLE_PIN_NUM_BK_LIGHT       45
#define EXAMPLE_PIN_NUM_TOUCH_CS       15

// The pixel number in horizontal and vertical
// #define EXAMPLE_LCD_H_RES              240
// #define EXAMPLE_LCD_V_RES              320
#define EXAMPLE_LCD_H_RES              320
#define EXAMPLE_LCD_V_RES              240
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS           8
#define EXAMPLE_LCD_PARAM_BITS         8

#define EXAMPLE_LVGL_DRAW_BUF_LINES    20 // number of display lines in each draw buffer
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (12 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2

#define LCD_HOST  SPI2_HOST

/* =======================================================================================================================================================*/
/* =======================================================================================================================================================*/
/* WiFi 信号等级定义 */
typedef enum {
    WIFI_SIGNAL_NONE  = 0,  /* 无信号 */
    WIFI_SIGNAL_WEAK  = 1,  /* 弱 */
    WIFI_SIGNAL_FAIR  = 2,  /* 一般 */
    WIFI_SIGNAL_GOOD  = 3,  /* 良好 */
    WIFI_SIGNAL_FULL  = 4,  /* 满格 */
} wifi_signal_level_t;
/* ============================================================
 *  配置参数
 * ============================================================ */
#define SCALE_FACTOR_NUM    3
#define SCALE_FACTOR_DEN    5

#define CANVAS_WIDTH        (60 * SCALE_FACTOR_NUM / SCALE_FACTOR_DEN)
#define CANVAS_HEIGHT       (52 * SCALE_FACTOR_NUM / SCALE_FACTOR_DEN)

/* 扇形圆心：画布底部中央 */
#define ARC_CENTER_X        (CANVAS_WIDTH / 2)
#define ARC_CENTER_Y        (CANVAS_HEIGHT - (4 * SCALE_FACTOR_NUM / SCALE_FACTOR_DEN))

/*
 * LVGL v9 角度说明：
 *   0.1度为单位，0度在3点钟方向，顺时针增大
 *   WiFi扇形朝上：起始225度，结束315度（均乘以10）
 */
#define ARC_START_ANGLE     225
#define ARC_END_ANGLE       315
#define ARC_WIDTH           (4 * SCALE_FACTOR_NUM / SCALE_FACTOR_DEN)
#define ARC_RADIUS_BASE     (8 * SCALE_FACTOR_NUM / SCALE_FACTOR_DEN)
#define ARC_RADIUS_STEP     (10 * SCALE_FACTOR_NUM / SCALE_FACTOR_DEN)
#define DOT_RADIUS          (3 * SCALE_FACTOR_NUM / SCALE_FACTOR_DEN)
// #define CANVAS_WIDTH        60
// #define CANVAS_HEIGHT       52

// /* 扇形圆心：画布底部中央 */
// #define ARC_CENTER_X        (CANVAS_WIDTH / 2)
// #define ARC_CENTER_Y        (CANVAS_HEIGHT - 4)

// /*
//  * LVGL v9 角度说明：
//  *   0.1度为单位，0度在3点钟方向，顺时针增大
//  *   WiFi扇形朝上：起始225度，结束315度（均乘以10）
//  */
// #define ARC_START_ANGLE     225
// #define ARC_END_ANGLE       315
// #define ARC_WIDTH           4
// #define ARC_RADIUS_BASE     8
// #define ARC_RADIUS_STEP     10
// #define DOT_RADIUS          3

/* 颜色定义 */
#define COLOR_ACTIVE        lv_color_make(0x00, 0xC8, 0x50)   /* 激活：绿色 */
#define COLOR_INACTIVE      lv_color_make(0x40, 0x40, 0x40)   /* 未激活：暗灰色 */
#define COLOR_BG            lv_color_make(0x1A, 0x1A, 0x1A)   /* 背景色 */
#define COLOR_TEXT_STRONG   lv_color_make(0x00, 0xC8, 0x50)   /* 强信号文字 */
#define COLOR_TEXT_WEAK     lv_color_make(0xFF, 0xA0, 0x00)   /* 弱信号文字 */
#define COLOR_TEXT_NONE     lv_color_make(0xFF, 0x40, 0x40)   /* 无信号文字 */

/* ============================================================
 *  内部状态
 * ============================================================ */
static lv_obj_t             *g_container   = NULL;
static lv_obj_t             *g_canvas      = NULL;
static lv_obj_t             *g_label       = NULL;
static lv_timer_t           *g_sim_timer   = NULL;
static lv_draw_buf_t         g_draw_buf;

static uint8_t g_canvas_buf[CANVAS_WIDTH * CANVAS_HEIGHT * 4];
static wifi_signal_level_t g_current_level = WIFI_SIGNAL_FULL;

/* 模拟器测试用：信号强度循环变化 */
static int g_sim_rssi = -55;
static int g_sim_direction = -5;
LV_FONT_DECLARE(MyFont);

/* 定义不同强度的 WiFi 图标 */
// #define WIFI_SYMBOL_STRONG  LV_SYMBOL_WIFI      /* 满格 */
// #define WIFI_SYMBOL_GOOD    "\xEF\x87\xAA"      /* 3格 */
// #define WIFI_SYMBOL_FAIR    "\xEF\x87\xA9"      /* 2格 */
// #define WIFI_SYMBOL_WEAK    "\xEF\x87\xA8"      /* 1格 */

static lv_obj_t * wifi_label = NULL;
static _lock_t lvgl_api_lock;


static const char *TAG = "WuHeYun example";

void wifi_signal_set_level(wifi_signal_level_t level);
void wifi_signal_set_rssi(int rssi);
void wifi_signal_widget_create(lv_obj_t *parent);


/**
 * @brief 在 Canvas 上重绘 WiFi 扇形图标
 */
static void wifi_signal_redraw(void)
{
    if (g_canvas == NULL) return;

    /* 清空画布为背景色 */
    lv_canvas_fill_bg(g_canvas, COLOR_BG, LV_OPA_COVER);

    /* 初始化图层 */
    lv_layer_t layer;
    lv_canvas_init_layer(g_canvas, &layer);

    /* 初始化绘制弧形的描述符 */
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.rounded = 1; // 设置圆角
    arc_dsc.width = ARC_WIDTH; // 弧线宽度

    /* 绘制 4 段扇形弧，从内到外 */
    for (int i = 0; i < 4; i++) {
        lv_coord_t radius = (lv_coord_t)(ARC_RADIUS_BASE + (i + 1) * ARC_RADIUS_STEP);

        /* 根据当前等级决定颜色 */
        if ((int)g_current_level > i) {
            arc_dsc.color = COLOR_ACTIVE; // 有信号的颜色
            arc_dsc.opa = LV_OPA_COVER; // 不透明
        } else {
            arc_dsc.color = COLOR_INACTIVE; // 无信号的颜色
            arc_dsc.opa = LV_OPA_40; // 半透明
        }

        /* 设置弧形参数 */
        arc_dsc.center.x = ARC_CENTER_X; // 圆心X
        arc_dsc.center.y = ARC_CENTER_Y; // 圆心Y
        arc_dsc.radius = radius; // 半径
        arc_dsc.start_angle = ARC_START_ANGLE; // 起始角度
        arc_dsc.end_angle = ARC_END_ANGLE; // 结束角度

        /* 绘制弧形 */
        lv_draw_arc(&layer, &arc_dsc);
    }

    /* 绘制中心圆点 */
    lv_draw_rect_dsc_t dot_dsc;
    lv_draw_rect_dsc_init(&dot_dsc);
    dot_dsc.radius = LV_RADIUS_CIRCLE; // 圆角设为圆型
    dot_dsc.bg_opa = LV_OPA_COVER; // 不透明
    dot_dsc.bg_color = (g_current_level > WIFI_SIGNAL_NONE)
                       ? COLOR_ACTIVE : COLOR_INACTIVE; // 中心颜色

    lv_area_t dot_area = {
        .x1 = ARC_CENTER_X - DOT_RADIUS,
        .y1 = ARC_CENTER_Y - DOT_RADIUS,
        .x2 = ARC_CENTER_X + DOT_RADIUS,
        .y2 = ARC_CENTER_Y + DOT_RADIUS,
    };

    lv_draw_rect(&layer, &dot_dsc, &dot_area); // 绘制中心点

    /* 完成图层绘制 */
    lv_canvas_finish_layer(g_canvas, &layer); // 完成绘制
}

/**
 * @brief 更新信号强度文字标签
 */
static void wifi_signal_update_label(void)
{
    if (g_label == NULL) return;

    char buf[24];
    lv_color_t txt_color;

    switch (g_current_level) {
        case WIFI_SIGNAL_NONE:
            snprintf(buf, sizeof(buf), "无信号");
            txt_color = COLOR_TEXT_NONE;
            break;
        case WIFI_SIGNAL_WEAK:
            snprintf(buf, sizeof(buf), "信号弱");
            txt_color = COLOR_TEXT_WEAK;
            break;
        case WIFI_SIGNAL_FAIR:
            snprintf(buf, sizeof(buf), "信号中");
            txt_color = COLOR_TEXT_WEAK;
            break;
        case WIFI_SIGNAL_GOOD:
            snprintf(buf, sizeof(buf), "信号好");
            txt_color = COLOR_TEXT_STRONG;
            break;
        case WIFI_SIGNAL_FULL:
            snprintf(buf, sizeof(buf), "信号强");
            txt_color = COLOR_TEXT_STRONG;
            break;
        default:
            snprintf(buf, sizeof(buf), "---");
            txt_color = COLOR_INACTIVE;
            break;
    }

    lv_label_set_text(g_label, buf);
    lv_obj_set_style_text_color(g_label, txt_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(g_label, &MyFont, 0);
}

/* ============================================================
 *  定时器回调（模拟器信号变化）
 * ============================================================ */
static void wifi_sim_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    /* 模拟 RSSI 在 -30 ~ -95 之间来回变化 */
    g_sim_rssi += g_sim_direction;
    if (g_sim_rssi <= -95) {
        g_sim_rssi     = -95;
        g_sim_direction = 5;
    } else if (g_sim_rssi >= -30) {
        g_sim_rssi     = -30;
        g_sim_direction = -5;
    }

    wifi_signal_set_rssi(g_sim_rssi);
}
/**
 * @brief 模拟获取 WiFi 信号强度
 * @return 信号强度 0-100
 */
// static int get_wifi_signal_strength(void)
// {
//     /* 这里应该调用你的实际 WiFi 驱动函数 */
//     /* 例如: return esp_wifi_get_rssi(); */

//     /* 模拟返回随机信号强度 */
//     static int signal = 75;
//     signal = (signal + 10) % 100;  /* 模拟变化 */
//     return signal;
// }

/**
 * @brief 根据信号强度更新 WiFi 图标和颜色
 * @param timer 定时器指针
 */
// static void update_wifi_indicator(lv_timer_t * timer)
// {
//     LV_UNUSED(timer);

//     if(wifi_label == NULL) return;

//     int signal = get_wifi_signal_strength();

//     /* 根据信号强度设置不同的 WiFi 图标 */
//     if(signal >= 75) {
//         /* 强信号 - 满格 WiFi - 绿色 */
//         lv_label_set_text(wifi_label, WIFI_SYMBOL_STRONG);
//         lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x00ff00), LV_PART_MAIN);
//     }
//     else if(signal >= 50) {
//         /* 中等信号 - 3格 WiFi - 黄色 */
//         lv_label_set_text(wifi_label, WIFI_SYMBOL_GOOD);
//         lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xffff00), LV_PART_MAIN);
//     }
//     else if(signal >= 25) {
//         /* 弱信号 - 2格 WiFi - 橙色 */
//         lv_label_set_text(wifi_label, WIFI_SYMBOL_FAIR);
//         lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xff8800), LV_PART_MAIN);
//     }
//     else if(signal > 0) {
//         /* 很弱信号 - 1格 WiFi - 红色 */
//         lv_label_set_text(wifi_label, WIFI_SYMBOL_WEAK);
//         lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xff0000), LV_PART_MAIN);
//     }
//     else {
//         /* 无信号 - 灰色，显示断开图标 */
//         lv_label_set_text(wifi_label, LV_SYMBOL_CLOSE);
//         lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x888888), LV_PART_MAIN);
//     }
// }

void wifi_signal_set_level(wifi_signal_level_t level)
{
    if (level > WIFI_SIGNAL_FULL) level = WIFI_SIGNAL_FULL;
    g_current_level = level;

    wifi_signal_redraw();
    wifi_signal_update_label();
}

void wifi_signal_set_rssi(int rssi)
{
    wifi_signal_level_t level;

    if      (rssi >= -55) level = WIFI_SIGNAL_FULL;
    else if (rssi >= -65) level = WIFI_SIGNAL_GOOD;
    else if (rssi >= -75) level = WIFI_SIGNAL_FAIR;
    else if (rssi >= -85) level = WIFI_SIGNAL_WEAK;
    else                  level = WIFI_SIGNAL_NONE;

    wifi_signal_set_level(level);
}

void wifi_signal_set_rssi_safe(int rssi)
{
    _lock_acquire(&lvgl_api_lock);
    wifi_signal_set_rssi(rssi);
    _lock_release(&lvgl_api_lock);
}

void wifi_signal_widget_create(lv_obj_t *parent)
{
    /* ------ 容器（右上角定位，透明背景） ------ */
    g_container = lv_obj_create(parent);
    lv_obj_set_size(g_container, CANVAS_WIDTH + 2, CANVAS_HEIGHT + 22);
    lv_obj_align(g_container, LV_ALIGN_TOP_RIGHT, -4, 2);

    /* 去除默认样式 */
    lv_obj_set_style_bg_opa(g_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(g_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_draw_buf_init(&g_draw_buf,
                     CANVAS_WIDTH,
                     CANVAS_HEIGHT,
                     LV_COLOR_FORMAT_ARGB8888,
                     LV_STRIDE_AUTO,
                     g_canvas_buf,
                     sizeof(g_canvas_buf));

    g_canvas = lv_canvas_create(g_container);
    lv_canvas_set_draw_buf(g_canvas, &g_draw_buf);
    lv_obj_align(g_canvas, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_canvas, LV_OPA_TRANSP, LV_PART_MAIN);

    /* ------ 文字标签（信号描述） ------ */
    // g_label = lv_label_create(g_container);
    // lv_obj_set_style_text_font(g_label,
    //                            &MyFont,
    //                            LV_PART_MAIN);
    // lv_label_set_text(g_label, "信号强");
    // lv_obj_set_style_text_color(g_label, COLOR_TEXT_STRONG, LV_PART_MAIN);
    // lv_obj_set_style_text_font(g_label, &MyFont, 0);
    // lv_obj_align(g_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 首次绘制 */
    wifi_signal_redraw();
    // wifi_signal_update_label();

    /* ------ 创建模拟器定时器（每 800ms 更新一次，模拟信号变化） ------ */
    /* 若在真实硬件上使用，删除此定时器，改为调用 wifi_signal_set_rssi() 即可 */
    g_sim_timer = lv_timer_create(wifi_sim_timer_cb, 2000, NULL);
}


/* =======================================================================================================================================================*/
/*
 * wuhe_main_ui — 五合云主界面
 *
 * 布局（屏幕 320×240，横屏）：
 *   ┌─────────────────────────────────────┐
 *   │  五合云                    [WiFi]   │  ← 标题行（顶部，高 36px）
 *   ├─────────────────────────────────────┤  ← 分割线
 *   │  仓库: [______]    岗位: [______]   │  ← 仓库/岗位行（y=38~78）
 *   ├─────────────────────────────────────┤  ← 分割线
 *   │  [条形码内容                      ] │  ← 扫码结果行（y=80~）
 *   └─────────────────────────────────────┘
 */

/* 主界面内部状态 */
static lv_obj_t *g_wuhe_warehouse_val = NULL;   /* 仓库号显示 label */
static lv_obj_t *g_wuhe_post_val      = NULL;   /* 岗位号显示 label */
static lv_obj_t *g_wuhe_barcode_val   = NULL;   /* 条形码内容显示 label */
static lv_obj_t *g_wuhe_workid_val    = NULL;   /* 工号显示 label */
static lv_obj_t *g_wuhe_count_val     = NULL;   /* 统计显示 label */

/**
 * @brief 更新仓库号，在任意位置调用
 * @param warehouse_str  仓库号字符串，如 "W-001"
 */
void wuhe_ui_set_warehouse(const char *warehouse_str)
{
    if (g_wuhe_warehouse_val == NULL) return;
    lv_label_set_text(g_wuhe_warehouse_val, warehouse_str);
}

/**
 * @brief 更新岗位号
 * @param post_str  岗位号字符串，如 "P-03"
 */
void wuhe_ui_set_post(const char *post_str)
{
    if (g_wuhe_post_val == NULL) return;
    lv_label_set_text(g_wuhe_post_val, post_str);
}

void wuhe_ui_set_warehouse_safe(const char *warehouse_str)
{
    if (warehouse_str == NULL) return;
    _lock_acquire(&lvgl_api_lock);
    wuhe_ui_set_warehouse(warehouse_str);
    _lock_release(&lvgl_api_lock);
}

void wuhe_ui_set_post_safe(const char *post_str)
{
    if (post_str == NULL) return;
    _lock_acquire(&lvgl_api_lock);
    wuhe_ui_set_post(post_str);
    _lock_release(&lvgl_api_lock);
}

/**
 * @brief 更新条形码扫描结果
 * @param barcode_str  扫描到的条形码内容
 */
void wuhe_ui_set_barcode(const char *barcode_str)
{
    if (g_wuhe_barcode_val == NULL) return;
    lv_label_set_text(g_wuhe_barcode_val, barcode_str);
}

void wuhe_ui_set_barcode_safe(const char *barcode_str)
{
    if (barcode_str == NULL) return;
    _lock_acquire(&lvgl_api_lock);
    wuhe_ui_set_barcode(barcode_str);
    _lock_release(&lvgl_api_lock);
}

/**
 * @brief 更新工号
 * @param workid_str  工号字符串
 */
void wuhe_ui_set_workid(const char *workid_str)
{
    if (g_wuhe_workid_val == NULL) return;
    lv_label_set_text(g_wuhe_workid_val, workid_str);
}

void wuhe_ui_set_workid_safe(const char *workid_str)
{
    if (workid_str == NULL) return;
    _lock_acquire(&lvgl_api_lock);
    wuhe_ui_set_workid(workid_str);
    _lock_release(&lvgl_api_lock);
}

/**
 * @brief 更新统计
 * @param count_str  统计字符串
 */
void wuhe_ui_set_count(const char *count_str)
{
    if (g_wuhe_count_val == NULL) return;
    lv_label_set_text(g_wuhe_count_val, count_str);
}

void wuhe_ui_set_count_safe(const char *count_str)
{
    if (count_str == NULL) return;
    _lock_acquire(&lvgl_api_lock);
    wuhe_ui_set_count(count_str);
    _lock_release(&lvgl_api_lock);
}


static void example_lvgl_port_update_callback(lv_display_t *disp)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);

    switch (rotation) {
    case LV_DISPLAY_ROTATION_0:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISPLAY_ROTATION_90:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, true);
        break;
    case LV_DISPLAY_ROTATION_180:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, true);
        break;
    case LV_DISPLAY_ROTATION_270:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, false);
        break;
    }
}


/**
 * @brief 创建五合云主界面，替换 lv_example_get_started_1 调用即可
 * @param disp  LVGL display 句柄
 */
void wuhe_main_ui(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);

    /* ── 屏幕背景 ── */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ── 标题 "五合云" ── */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "五合云");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &MyFont, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 10);

    /* ── WiFi 信号 widget（右上角，复用现有实现） ── */
    wifi_signal_widget_create(scr);

    /* ── 第一条分割线（标题下方，y=36） ── */
    lv_obj_t *line1 = lv_obj_create(scr);
    lv_obj_set_size(line1, 310, 2);
    lv_obj_align(line1, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(line1, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line1, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line1, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line1, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 仓库/岗位行（y=48） ── */

    /* "仓库：" 静态文字，左侧 */
    lv_obj_t *lbl_warehouse = lv_label_create(scr);
    lv_label_set_text(lbl_warehouse, "仓库：");
    lv_obj_set_style_text_color(lbl_warehouse, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_warehouse, &MyFont, LV_PART_MAIN);
    lv_obj_align(lbl_warehouse, LV_ALIGN_TOP_LEFT, 8, 48);

    /* 仓库号动态值，紧接 "仓库：" 右侧 */
    g_wuhe_warehouse_val = lv_label_create(scr);
    lv_label_set_text(g_wuhe_warehouse_val, "--");
    lv_obj_set_style_text_color(g_wuhe_warehouse_val, lv_color_hex(0x00C850), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_wuhe_warehouse_val, &MyFont, LV_PART_MAIN);
    lv_obj_align_to(g_wuhe_warehouse_val, lbl_warehouse, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    /* "岗位：" 静态文字，屏幕水平中点偏右 */
    lv_obj_t *lbl_post = lv_label_create(scr);
    lv_label_set_text(lbl_post, "岗位：");
    lv_obj_set_style_text_color(lbl_post, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_post, &MyFont, LV_PART_MAIN);
    lv_obj_align(lbl_post, LV_ALIGN_TOP_MID, 10, 48);

    /* 岗位号动态值，紧接 "岗位：" 右侧 */
    g_wuhe_post_val = lv_label_create(scr);
    lv_label_set_text(g_wuhe_post_val, "--");
    lv_obj_set_style_text_color(g_wuhe_post_val, lv_color_hex(0x00C850), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_wuhe_post_val, &MyFont, LV_PART_MAIN);
    lv_obj_align_to(g_wuhe_post_val, lbl_post, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    /* ── 第二条分割线（仓库/岗位行下方，y=78） ── */
    lv_obj_t *line2 = lv_obj_create(scr);
    lv_obj_set_size(line2, 310, 2);
    lv_obj_align(line2, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_bg_color(line2, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line2, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line2, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 工号/统计行（y=88） ── */

    /* "工号：" 静态文字，左侧 */
    lv_obj_t *lbl_workid = lv_label_create(scr);
    lv_label_set_text(lbl_workid, "工号：");
    lv_obj_set_style_text_color(lbl_workid, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_workid, &MyFont, LV_PART_MAIN);
    lv_obj_align(lbl_workid, LV_ALIGN_TOP_LEFT, 8, 88);

    /* 工号动态值，紧接 "工号：" 右侧 */
    g_wuhe_workid_val = lv_label_create(scr);
    lv_label_set_text(g_wuhe_workid_val, "--");
    lv_obj_set_style_text_color(g_wuhe_workid_val, lv_color_hex(0x00C850), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_wuhe_workid_val, &MyFont, LV_PART_MAIN);
    lv_obj_align_to(g_wuhe_workid_val, lbl_workid, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    /* "统计：" 静态文字，屏幕水平中点偏右 */
    lv_obj_t *lbl_count = lv_label_create(scr);
    lv_label_set_text(lbl_count, "统计：");
    lv_obj_set_style_text_color(lbl_count, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_count, &MyFont, LV_PART_MAIN);
    lv_obj_align(lbl_count, LV_ALIGN_TOP_MID, 10, 88);

    /* 统计动态值，紧接 "统计：" 右侧 */
    g_wuhe_count_val = lv_label_create(scr);
    lv_label_set_text(g_wuhe_count_val, "--");
    lv_obj_set_style_text_color(g_wuhe_count_val, lv_color_hex(0x00C850), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_wuhe_count_val, &MyFont, LV_PART_MAIN);
    lv_obj_align_to(g_wuhe_count_val, lbl_count, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    /* ── 第三条分割线（工号/统计行下方，y=118） ── */
    lv_obj_t *line3 = lv_obj_create(scr);
    lv_obj_set_size(line3, 310, 2);
    lv_obj_align(line3, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(line3, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line3, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line3, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line3, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 条形码扫描结果行（y=130） ── */
    /* 外框容器，带圆角边框，高度 40px */
    lv_obj_t *barcode_box = lv_obj_create(scr);
    lv_obj_set_size(barcode_box, 304, 40);
    lv_obj_align(barcode_box, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_color(barcode_box, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(barcode_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(barcode_box, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_border_width(barcode_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(barcode_box, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(barcode_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(barcode_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(barcode_box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(barcode_box, LV_OBJ_FLAG_SCROLLABLE);

    /* 条形码内容 label，超长时循环滚动 */
    g_wuhe_barcode_val = lv_label_create(barcode_box);
    lv_label_set_text(g_wuhe_barcode_val, "等待扫码...");
    lv_label_set_long_mode(g_wuhe_barcode_val, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(g_wuhe_barcode_val, 292);
    lv_obj_set_style_text_color(g_wuhe_barcode_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_wuhe_barcode_val, &MyFont, LV_PART_MAIN);
    lv_obj_align(g_wuhe_barcode_val, LV_ALIGN_LEFT_MID, 0, 0);
}

static void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    example_lvgl_port_update_callback(disp);
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // because SPI LCD is big-endian, we need to swap the RGB bytes order
    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

static void example_lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        // in case of triggering a task watch dog time out
        time_till_next_ms = MAX(time_till_next_ms, EXAMPLE_LVGL_TASK_MIN_DELAY_MS);
        // in case of lvgl display not ready yet
        time_till_next_ms = MIN(time_till_next_ms, EXAMPLE_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}


void display_start_up_pagevoid(lv_display_t *disp)
{
    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_PIN_NUM_SCLK,
        .mosi_io_num = EXAMPLE_PIN_NUM_MOSI,
        .miso_io_num = EXAMPLE_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_PIN_NUM_LCD_DC,
        .cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    // ESP_LOGI(TAG, "Install ST7789 panel driver");
    // ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    // ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    // ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_LOGI(TAG, "Install ILI9341 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));


    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));

    // user can flush pre-defined pattern to the screen before we turn on the screen or backlight
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    // create a lvgl display
    lv_display_t *display = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);

    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    size_t draw_buffer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);

    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);
    // initialize LVGL draw buffers
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    // associate the mipi panel handle to the display
    lv_display_set_user_data(display, panel_handle);
    // set color depth
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    // set the callback which can copy the rendered image to an area of the display
    lv_display_set_flush_cb(display, example_lvgl_flush_cb);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = example_notify_lvgl_flush_ready,
    };
    /* Register done callback */
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_config =
#ifdef CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_STMPE610
        ESP_LCD_TOUCH_IO_SPI_STMPE610_CONFIG(EXAMPLE_PIN_NUM_TOUCH_CS);
#elif CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_XPT2046
        ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(EXAMPLE_PIN_NUM_TOUCH_CS);
#endif
    // Attach the TOUCH to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = CONFIG_EXAMPLE_LCD_MIRROR_Y,
        },
    };
    esp_lcd_touch_handle_t tp = NULL;

#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_STMPE610
    ESP_LOGI(TAG, "Initialize touch controller STMPE610");
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_stmpe610(tp_io_handle, &tp_cfg, &tp));
#elif CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_XPT2046
    ESP_LOGI(TAG, "Initialize touch controller XPT2046");
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &tp));
#endif

    static lv_indev_t *indev;
    indev = lv_indev_create(); // Input device driver (Touch)
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_user_data(indev, tp);
    lv_indev_set_read_cb(indev, example_lvgl_touch_cb);
#endif

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Display LVGL Meter Widget");
    // Lock the mutex due to the LVGL APIs are not thread-safe
    _lock_acquire(&lvgl_api_lock);
    // example_lvgl_demo_ui(display);
    wuhe_main_ui(display);
    _lock_release(&lvgl_api_lock);
}



