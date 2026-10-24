// ============================================================================
//  media_display.ino
//  RP2040 + ST7789V (320x240), LovyanGFX + LVGL 9.6, связь с ПК по USB CDC.
//
//  Экран:  [ иконки сессий 60x240 ][ обложка, название, таймлайн 260x240 ]
//  Кнопки: BTN_UP / BTN_DOWN переключают выбранную медиасессию.
//
//  Требования к lv_conf.h:
//    LV_COLOR_DEPTH 16, LV_USE_BAR 1, LV_USE_LABEL 1,
//    LV_MEM_SIZE >= 48 * 1024 (при LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN)
// ============================================================================
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

LV_FONT_DECLARE(my_font_16);  // кастомный шрифт с кириллицей
#define UI_FONT (&my_font_16)

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel_instance;
    lgfx::Bus_SPI       _bus_instance;
public:
    LGFX(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = 0;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.pin_sclk = 18;
            cfg.pin_mosi = 19;
            cfg.pin_miso = -1;
            cfg.pin_dc   = 16;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 17;
            cfg.pin_rst          = 20;
            cfg.panel_width      = 240;
            cfg.panel_height     = 320;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.rgb_order        = false;
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
};
LGFX lcd;

// ----------------------------------------------------------------------------
//  Настройки
// ----------------------------------------------------------------------------
constexpr uint16_t SCREEN_W = 320;
constexpr uint16_t SCREEN_H = 240;
constexpr uint16_t LIST_W   = 60;
constexpr uint16_t RIGHT_W  = SCREEN_W - LIST_W;   // 260

constexpr uint8_t  MAX_SESSIONS = 16;
constexpr uint16_t COVER_SIZE   = 128;
constexpr uint16_t ICON_SIZE    = 32;
constexpr uint16_t MAX_PAYLOAD  = 6 + COVER_SIZE * COVER_SIZE * 2;   // 32774

constexpr uint8_t BTN_UP_PIN   = 14;   // кнопки на GND, внутренняя подтяжка
constexpr uint8_t BTN_DOWN_PIN = 15;

// ----------------------------------------------------------------------------
//  Протокол
// ----------------------------------------------------------------------------
constexpr uint8_t PKT_H1 = 0xAA;
constexpr uint8_t PKT_H2 = 0xBB;

enum CommandID : uint8_t {
    CMD_CONFIGURATION          = 0x01,
    CMD_CREATE_MEDIASESSION    = 0x02,
    CMD_DELETE_MEDIASESSION    = 0x03,
    CMD_SELECT_MEDIASESSION    = 0x04,
    CMD_TRACK_NAME             = 0x05,
    CMD_TRACK_ARTIST           = 0x06,
    CMD_TRACK_ALBUM            = 0x07,
    CMD_TRACK_CURRENT_TIME     = 0x08,
    CMD_TRACK_DURATION         = 0x09,
    CMD_TRACK_STATE            = 0x0A,
    CMD_TRACK_PHOTO            = 0x0B,
    CMD_APP_ICON               = 0x0C,
    CMD_APPS_BACKGROUND_COLOR  = 0x0D,   // RGB565, big-endian
    CMD_TRACK_BACKGROUND_COLOR = 0x0E,   // RGB565, big-endian
};

// ---- CRC8 (ВАЖНО: подгоните под вашу реализацию на ПК!) --------------------
// Здесь: полином 0x07, init 0, считается по cmd + len(hi,lo) + payload.
static uint8_t crcTable[256];

static void crcInit() {
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)i;
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
        crcTable[i] = c;
    }
}

static uint8_t packetCrc(uint8_t cmd, uint16_t len, const uint8_t *payload) {
    uint8_t c = 0;
    c = crcTable[c ^ cmd];
    c = crcTable[c ^ (uint8_t)(len >> 8)];
    c = crcTable[c ^ (uint8_t)(len & 0xFF)];
    for (uint16_t i = 0; i < len; i++) c = crcTable[c ^ payload[i]];
    return c;
}

static void sendPacket(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    uint8_t hdr[5] = { PKT_H1, PKT_H2, cmd, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
    Serial.write(hdr, sizeof(hdr));
    if (len) Serial.write(payload, len);
    Serial.write(packetCrc(cmd, len, payload));
}

// ----------------------------------------------------------------------------
//  Данные
// ----------------------------------------------------------------------------
struct MediaSession {
    char     AppName[48] = "";
    uint16_t bgColor     = 0;        // 0 = не задан
    bool     hasIcon     = false;
    alignas(4) uint16_t icon[ICON_SIZE * ICON_SIZE];
    bool used() const { return AppName[0] != '\0'; }
};
static MediaSession sessions[MAX_SESSIONS];
static int8_t selectedId = -1;

struct CurrentTrack {                // данные только выбранной сессии
    char     title[128];
    char     artist[128];
    char     album[128];
    uint32_t timeSec;
    uint32_t durationSec;
    uint8_t  state;                  // 0 stop, 1 play, 2 pause, 3 error
    uint16_t bgColor;                // 0 = не задан
    bool     hasCover;
};
static CurrentTrack cur;
alignas(4) static uint16_t cover[COVER_SIZE * COVER_SIZE];   // 32 КБ

static uint32_t lastTickMs = 0;      // локальное досчитывание времени

// ----------------------------------------------------------------------------
//  Утилиты
// ----------------------------------------------------------------------------
static uint16_t rd16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// копирует строку из пакета (без \0), не обрезая посреди UTF-8 символа
static void copyStr(char *dst, size_t dstSize, const uint8_t *src, size_t len) {
    size_t n = len;
    if (n > dstSize - 1) {
        n = dstSize - 1;
        while (n > 0 && (src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool isSelected(const uint8_t *p, uint16_t len) {
    return len >= 1 && selectedId >= 0 && p[0] == (uint8_t)selectedId;
}

// payload: [id][w hi][w lo][h hi][h lo][format=0][RGB565 BE ...]
static bool decodeImage(const uint8_t *p, uint16_t len, uint16_t expW, uint16_t expH, uint16_t *dst) {
    if (len < 6) return false;
    if (rd16(p + 1) != expW || rd16(p + 3) != expH || p[5] != 0) return false;
    size_t px = (size_t)expW * expH;
    if (len < 6 + px * 2) return false;
    const uint8_t *s = p + 6;
    for (size_t i = 0; i < px; i++) dst[i] = ((uint16_t)s[2 * i] << 8) | s[2 * i + 1];
    return true;
}

static lv_color_t rgb565ToLv(uint16_t c) {
    uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint8_t g = ((c >> 5)  & 0x3F) * 255 / 63;
    uint8_t b = (c & 0x1F) * 255 / 31;
    return lv_color_make(r, g, b);
}

static bool isLightColor(uint16_t c) {
    uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
    uint32_t b = (c & 0x1F) * 255 / 31;
    return (r * 299 + g * 587 + b * 114) / 1000 > 140;
}

static void fmtTime(char *out, size_t n, uint32_t s) {
    if (s >= 3600) snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
    else           snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// первый символ UTF-8 строки (кириллица = 2 байта); out >= 5 байт
static void firstChar(const char *s, char *out) {
    unsigned char c = (unsigned char)s[0];
    size_t n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    n = strnlen(s, n);
    memcpy(out, s, n);
    out[n] = '\0';
}

static void fillDsc(lv_image_dsc_t &d, const uint16_t *data, uint16_t w, uint16_t h) {
    memset(&d, 0, sizeof(d));
    d.header.magic  = LV_IMAGE_HEADER_MAGIC;
    d.header.cf     = LV_COLOR_FORMAT_RGB565;
    d.header.w      = w;
    d.header.h      = h;
    d.header.stride = w * 2;
    d.data_size     = (uint32_t)w * h * 2;
    d.data          = (const uint8_t *)data;
}

// ----------------------------------------------------------------------------
//  UI (LVGL 9.6)
// ----------------------------------------------------------------------------
static lv_obj_t *appList, *rightPanel;
static lv_obj_t *titleLbl, *artistLbl;
static lv_obj_t *coverImg, *coverPlaceholder;
static lv_obj_t *dimOverlay, *pauseIcon;      // затемнение и знак «||» поверх обложки
static lv_obj_t *progressBar, *timeLbl, *durLbl, *appLbl;
static lv_obj_t *offlineOverlay;              // «Сервер не запущен»

static lv_image_dsc_t coverDsc;
static lv_image_dsc_t iconDsc[MAX_SESSIONS];

static bool    serverUp   = false;            // ПК-программа подключена (DTR)
static uint8_t pauseLevel = 0;                // 0..255: текущая «глубина» паузы

constexpr uint8_t  PAUSE_DIM_OPA = 170;       // насколько темнеет обложка (из 255)
constexpr uint32_t PAUSE_ANIM_MS = 250;       // длительность анимации паузы

// кадр анимации: v = 0 (играет) ... 255 (пауза)
static void pauseAnimCb(void *var, int32_t v) {
    (void)var;
    pauseLevel = (uint8_t)v;
    if (v <= 0) {                             // скрытые объекты не рисуются и не нагружают экран
        lv_obj_add_flag(dimOverlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pauseIcon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(dimOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(pauseIcon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(dimOverlay, (lv_opa_t)(v * PAUSE_DIM_OPA / 255), 0);
    lv_obj_set_style_opa(pauseIcon, (lv_opa_t)v, 0);
}

static void setPauseVisual(bool paused, bool animate) {
    int32_t target = paused ? 255 : 0;
    lv_anim_delete(dimOverlay, pauseAnimCb);      // прервать текущую анимацию
    if (!animate) { pauseAnimCb(dimOverlay, target); return; }
    if (pauseLevel == target) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dimOverlay);
    lv_anim_set_exec_cb(&a, pauseAnimCb);
    lv_anim_set_values(&a, pauseLevel, target);   // с текущего уровня, без скачка
    lv_anim_set_duration(&a, PAUSE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void ui_init() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int lh = lv_font_get_line_height(UI_FONT);

    // ---------------- левая панель: иконки сессий ----------------
    appList = lv_obj_create(scr);
    lv_obj_set_size(appList, LIST_W, SCREEN_H);
    lv_obj_set_pos(appList, 0, 0);
    lv_obj_set_style_radius(appList, 0, 0);
    lv_obj_set_style_border_width(appList, 0, 0);
    lv_obj_set_style_bg_color(appList, lv_color_hex(0x101010), 0);
    lv_obj_set_style_pad_all(appList, 3, 0);
    lv_obj_set_style_pad_row(appList, 3, 0);
    lv_obj_set_scrollbar_mode(appList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(appList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(appList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // ---------------- правая панель ----------------
    rightPanel = lv_obj_create(scr);
    lv_obj_set_size(rightPanel, RIGHT_W, SCREEN_H);
    lv_obj_set_pos(rightPanel, LIST_W, 0);
    lv_obj_set_style_radius(rightPanel, 0, 0);
    lv_obj_set_style_border_width(rightPanel, 0, 0);
    lv_obj_set_style_bg_color(rightPanel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rightPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rightPanel, 4, 0);
    lv_obj_remove_flag(rightPanel, LV_OBJ_FLAG_SCROLLABLE);

    // сверху вниз: обложка, название, исполнитель, таймлайн, строка времени
    const int innerW  = RIGHT_W - 8;                 // 252
    const int coverY  = 0;
    const int titleY  = coverY + COVER_SIZE + 6;
    const int artistY = titleY + lh + 2;
    const int barY    = artistY + lh + 8;
    const int timeY   = barY + 6 + 4;

    // обложка (рисуется прямо из массива cover[])
    fillDsc(coverDsc, cover, COVER_SIZE, COVER_SIZE);
    coverImg = lv_image_create(rightPanel);
    lv_image_set_src(coverImg, &coverDsc);
    lv_obj_align(coverImg, LV_ALIGN_TOP_MID, 0, coverY);
    lv_obj_add_flag(coverImg, LV_OBJ_FLAG_HIDDEN);

    // заглушка, пока обложки нет
    coverPlaceholder = lv_obj_create(rightPanel);
    lv_obj_set_size(coverPlaceholder, COVER_SIZE, COVER_SIZE);
    lv_obj_align(coverPlaceholder, LV_ALIGN_TOP_MID, 0, coverY);
    lv_obj_set_style_bg_color(coverPlaceholder, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_width(coverPlaceholder, 0, 0);
    lv_obj_set_style_radius(coverPlaceholder, 8, 0);
    lv_obj_remove_flag(coverPlaceholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ph = lv_label_create(coverPlaceholder);
    lv_label_set_text(ph, "Нет обложки");
    lv_obj_set_style_text_font(ph, UI_FONT, 0);
    lv_obj_set_style_text_color(ph, lv_color_hex(0x707070), 0);
    lv_obj_center(ph);

    // затемнение обложки при паузе (поверх обложки и заглушки)
    dimOverlay = lv_obj_create(rightPanel);
    lv_obj_set_size(dimOverlay, COVER_SIZE, COVER_SIZE);
    lv_obj_align(dimOverlay, LV_ALIGN_TOP_MID, 0, coverY);
    lv_obj_set_style_radius(dimOverlay, 0, 0);
    lv_obj_set_style_border_width(dimOverlay, 0, 0);
    lv_obj_set_style_bg_color(dimOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dimOverlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(dimOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dimOverlay, LV_OBJ_FLAG_HIDDEN);

    // знак паузы «||» по центру обложки: две полоски
    const int barW = 11, barH = 38, barGap = 12;
    pauseIcon = lv_obj_create(rightPanel);
    lv_obj_set_size(pauseIcon, barW * 2 + barGap, barH);
    lv_obj_align(pauseIcon, LV_ALIGN_TOP_MID, 0, coverY + (COVER_SIZE - barH) / 2);
    lv_obj_set_style_bg_opa(pauseIcon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pauseIcon, 0, 0);
    lv_obj_set_style_pad_all(pauseIcon, 0, 0);
    lv_obj_remove_flag(pauseIcon, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = lv_obj_create(pauseIcon);
        lv_obj_set_size(bar, barW, barH);
        lv_obj_set_pos(bar, i * (barW + barGap), 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_flag(pauseIcon, LV_OBJ_FLAG_HIDDEN);

    // название и исполнитель над таймлайном
    titleLbl = lv_label_create(rightPanel);
    lv_obj_set_width(titleLbl, innerW);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(titleLbl, UI_FONT, 0);
    lv_obj_set_style_text_align(titleLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, titleY);

    artistLbl = lv_label_create(rightPanel);
    lv_obj_set_width(artistLbl, innerW);
    lv_label_set_long_mode(artistLbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(artistLbl, UI_FONT, 0);
    lv_obj_set_style_text_align(artistLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(artistLbl, LV_OPA_80, 0);
    lv_obj_align(artistLbl, LV_ALIGN_TOP_MID, 0, artistY);

    // полоса прогресса
    progressBar = lv_bar_create(rightPanel);
    lv_obj_set_size(progressBar, innerW, 6);
    lv_obj_set_pos(progressBar, 0, barY);
    lv_bar_set_range(progressBar, 0, 1000);
    lv_bar_set_value(progressBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progressBar, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progressBar, lv_color_white(), LV_PART_INDICATOR);

    // время слева / название приложения по центру / длительность справа
    timeLbl = lv_label_create(rightPanel);
    lv_obj_set_style_text_font(timeLbl, UI_FONT, 0);
    lv_obj_align(timeLbl, LV_ALIGN_TOP_LEFT, 0, timeY);

    appLbl = lv_label_create(rightPanel);
    lv_obj_set_size(appLbl, 120, lh);                       // для режима DOTS нужен фиксированный размер
    lv_label_set_long_mode(appLbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(appLbl, UI_FONT, 0);
    lv_obj_set_style_text_align(appLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(appLbl, LV_OPA_80, 0);
    lv_obj_align(appLbl, LV_ALIGN_TOP_MID, 0, timeY);

    durLbl = lv_label_create(rightPanel);
    lv_obj_set_style_text_font(durLbl, UI_FONT, 0);
    lv_obj_align(durLbl, LV_ALIGN_TOP_RIGHT, 0, timeY);

    // ---------------- «Сервер не запущен» (поверх всего) ----------------
    offlineOverlay = lv_obj_create(scr);
    lv_obj_set_size(offlineOverlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(offlineOverlay, 0, 0);
    lv_obj_set_style_radius(offlineOverlay, 0, 0);
    lv_obj_set_style_border_width(offlineOverlay, 0, 0);
    lv_obj_set_style_bg_color(offlineOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(offlineOverlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(offlineOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *plate = lv_obj_create(offlineOverlay);       // та же плашка, что «Нет обложки»
    lv_obj_set_size(plate, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(plate, 20, 0);
    lv_obj_set_style_pad_ver(plate, 14, 0);
    lv_obj_set_style_bg_color(plate, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(plate, 0, 0);
    lv_obj_set_style_radius(plate, 8, 0);
    lv_obj_remove_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(plate);
    lv_obj_t *off = lv_label_create(plate);
    lv_label_set_text(off, "Сервер не запущен");
    lv_obj_set_style_text_font(off, UI_FONT, 0);
    lv_obj_set_style_text_color(off, lv_color_hex(0x707070), 0);
}

// ---- список сессий (только иконки) ----
static void refreshAppList() {
    lv_obj_clean(appList);
    lv_obj_t *selectedItem = nullptr;

    for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
        MediaSession &s = sessions[i];
        if (!s.used()) continue;

        bool sel = ((int8_t)i == selectedId);

        lv_obj_t *item = lv_obj_create(appList);
        lv_obj_set_size(item, LIST_W - 6, ICON_SIZE + 12);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(item, 2, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_bg_color(item, s.bgColor ? rgb565ToLv(s.bgColor) : lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(item, sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(item, lv_color_white(), 0);
        if (sel) selectedItem = item;

        // иконка либо кружок с первой буквой
        if (s.hasIcon) {
            fillDsc(iconDsc[i], s.icon, ICON_SIZE, ICON_SIZE);
            lv_obj_t *img = lv_image_create(item);
            lv_image_set_src(img, &iconDsc[i]);
            lv_obj_center(img);
        } else {
            lv_obj_t *circle = lv_obj_create(item);
            lv_obj_set_size(circle, ICON_SIZE, ICON_SIZE);
            lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(circle, lv_color_hex(0x555555), 0);
            lv_obj_set_style_border_width(circle, 0, 0);
            lv_obj_set_style_pad_all(circle, 0, 0);
            lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_center(circle);

            char ch[5];
            firstChar(s.AppName, ch);
            lv_obj_t *l = lv_label_create(circle);
            lv_label_set_text(l, ch);
            lv_obj_set_style_text_font(l, UI_FONT, 0);
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
            lv_obj_center(l);
        }
    }

    if (selectedItem) {
        lv_obj_update_layout(appList);
        lv_obj_scroll_to_view(selectedItem, LV_ANIM_ON);
    }
}

// ---- правая панель ----
static void ui_trackTextChanged() {
    if (selectedId < 0) {
        lv_label_set_text(titleLbl, "Нет активных приложений");
        lv_label_set_text(artistLbl, "");
        return;
    }
    lv_label_set_text(titleLbl, cur.title);
    lv_label_set_text(artistLbl, cur.artist);
}

static void ui_coverChanged() {
    if (cur.hasCover && selectedId >= 0) {
        lv_obj_add_flag(coverPlaceholder, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(coverImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(dimOverlay, 0, 0);      // у обложки углы прямые
        lv_obj_invalidate(coverImg);
    } else {
        lv_obj_add_flag(coverImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(coverPlaceholder, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(dimOverlay, 8, 0);      // как у заглушки
    }
}

static void ui_timeChanged() {
    char buf[16];
    // нет выбранной сессии или у трека нет длительности:
    // время не показываем, таймлайн остаётся пустым
    if (selectedId < 0 || cur.durationSec == 0) {
        lv_label_set_text(timeLbl, "");
        lv_label_set_text(durLbl, "");
        lv_bar_set_value(progressBar, 0, LV_ANIM_OFF);
        return;
    }
    fmtTime(buf, sizeof(buf), cur.timeSec);
    lv_label_set_text(timeLbl, buf);
    fmtTime(buf, sizeof(buf), cur.durationSec);
    lv_label_set_text(durLbl, buf);

    uint32_t t = cur.timeSec > cur.durationSec ? cur.durationSec : cur.timeSec;
    lv_bar_set_value(progressBar, (int32_t)((uint64_t)t * 1000 / cur.durationSec), LV_ANIM_OFF);
}

// пауза: обложка плавно темнеет, поверх появляется «||» (state 2 = пауза)
static void ui_stateChanged(bool animate = true) {
    setPauseVisual(selectedId >= 0 && cur.state == 2, animate);
}

static void ui_appNameChanged() {
    lv_label_set_text(appLbl, (selectedId >= 0 && sessions[selectedId].used()) ? sessions[selectedId].AppName : "");
}

static void ui_serverStateChanged() {
    if (serverUp) lv_obj_add_flag(offlineOverlay, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_remove_flag(offlineOverlay, LV_OBJ_FLAG_HIDDEN);
}

static void ui_bgChanged() {
    bool custom = (selectedId >= 0 && cur.bgColor != 0);
    lv_obj_set_style_bg_color(rightPanel, custom ? rgb565ToLv(cur.bgColor) : lv_color_black(), 0);
    lv_color_t txt = (custom && isLightColor(cur.bgColor)) ? lv_color_black() : lv_color_white();
    lv_obj_set_style_text_color(titleLbl, txt, 0);
    lv_obj_set_style_text_color(artistLbl, txt, 0);
    lv_obj_set_style_text_color(timeLbl, txt, 0);
    lv_obj_set_style_text_color(durLbl, txt, 0);
    lv_obj_set_style_text_color(appLbl, txt, 0);
    lv_obj_set_style_bg_color(progressBar, txt, LV_PART_INDICATOR);
}

static void ui_selectionChanged() {
    refreshAppList();
    ui_trackTextChanged();
    ui_coverChanged();
    ui_timeChanged();
    ui_stateChanged(false);               // без анимации: состояние выбранной сессии сброшено
    ui_appNameChanged();
    ui_bgChanged();
}

// ----------------------------------------------------------------------------
//  Выбор сессии
// ----------------------------------------------------------------------------
static void selectSession(int8_t id, bool notifyPc) {
    if (id == selectedId) return;
    selectedId = id;
    cur = CurrentTrack{};                 // не показываем данные прошлого трека
    lastTickMs = millis();
    ui_selectionChanged();
    if (notifyPc && id >= 0) {
        uint8_t p[1] = { (uint8_t)id };
        sendPacket(CMD_SELECT_MEDIASESSION, p, 1);
    }
}

// dir = -1 (выше) / +1 (ниже); пропускает пустые слоты, по кругу
static void selectRelative(int8_t dir) {
    for (int step = 1; step <= MAX_SESSIONS; step++) {
        int idx = (((selectedId + dir * step) % MAX_SESSIONS) + MAX_SESSIONS) % MAX_SESSIONS;
        if (sessions[idx].used()) {
            selectSession((int8_t)idx, true);
            return;
        }
    }
}

// ----------------------------------------------------------------------------
//  Обработка пакетов
// ----------------------------------------------------------------------------
static void processPacket(uint8_t cmd, const uint8_t *payload, uint16_t length) {
    switch (cmd) {
        case CMD_CONFIGURATION:
            // зарезервировано под ваши настройки
            break;

        case CMD_CREATE_MEDIASESSION: {
            if (length < 1 || payload[0] >= MAX_SESSIONS) break;
            MediaSession &s = sessions[payload[0]];
            copyStr(s.AppName, sizeof(s.AppName), payload + 1, length - 1);
            if (!s.used()) break;                       // пустое имя игнорируем
            refreshAppList();
            ui_appNameChanged();
            if (selectedId < 0) selectSession((int8_t)payload[0], true);   // первая сессия выбирается сама
            break;
        }

        case CMD_DELETE_MEDIASESSION: {
            if (length < 1 || payload[0] >= MAX_SESSIONS) break;
            MediaSession &s = sessions[payload[0]];
            s.AppName[0] = '\0';
            s.hasIcon = false;
            s.bgColor = 0;
            if (selectedId == (int8_t)payload[0]) {
                selectedId = -1;
                cur = CurrentTrack{};
                ui_selectionChanged();
                selectRelative(+1);                     // перейти на ближайшую живую
            } else {
                refreshAppList();
            }
            break;
        }

        case CMD_SELECT_MEDIASESSION:                   // принудительная смена со стороны ПК
            if (length >= 1 && payload[0] < MAX_SESSIONS && sessions[payload[0]].used())
                selectSession((int8_t)payload[0], false);
            break;

        case CMD_TRACK_NAME:
            if (!isSelected(payload, length)) break;
            copyStr(cur.title, sizeof(cur.title), payload + 1, length - 1);
            ui_trackTextChanged();
            break;

        case CMD_TRACK_ARTIST:
            if (!isSelected(payload, length)) break;
            copyStr(cur.artist, sizeof(cur.artist), payload + 1, length - 1);
            ui_trackTextChanged();
            break;

        case CMD_TRACK_ALBUM:                           // хранится, но на экране не показывается
            if (!isSelected(payload, length)) break;
            copyStr(cur.album, sizeof(cur.album), payload + 1, length - 1);
            break;

        case CMD_TRACK_CURRENT_TIME:
            if (!isSelected(payload, length) || length < 5) break;
            cur.timeSec = rd32(payload + 1);
            lastTickMs = millis();
            ui_timeChanged();
            break;

        case CMD_TRACK_DURATION:
            if (!isSelected(payload, length) || length < 5) break;
            cur.durationSec = rd32(payload + 1);
            ui_timeChanged();
            break;

        case CMD_TRACK_STATE:
            if (!isSelected(payload, length) || length < 2) break;
            cur.state = payload[1] <= 3 ? payload[1] : 3;
            lastTickMs = millis();
            ui_stateChanged();
            break;

        case CMD_TRACK_PHOTO:
            if (!isSelected(payload, length)) break;
            if (length == 1) {                          // только [id]: у трека нет обложки
                cur.hasCover = false;
                ui_coverChanged();                      // покажет заглушку «Нет обложки»
            } else if (decodeImage(payload, length, COVER_SIZE, COVER_SIZE, cover)) {
                cur.hasCover = true;
                ui_coverChanged();
            }
            break;

        case CMD_APP_ICON: {                            // иконки приходят для любой сессии
            if (length < 1 || payload[0] >= MAX_SESSIONS) break;
            MediaSession &s = sessions[payload[0]];
            if (s.used() && decodeImage(payload, length, ICON_SIZE, ICON_SIZE, s.icon)) {
                s.hasIcon = true;
                refreshAppList();
            }
            break;
        }

        case CMD_APPS_BACKGROUND_COLOR:
            if (length < 3 || payload[0] >= MAX_SESSIONS) break;
            sessions[payload[0]].bgColor = rd16(payload + 1);
            refreshAppList();
            break;

        case CMD_TRACK_BACKGROUND_COLOR:
            if (!isSelected(payload, length) || length < 3) break;
            cur.bgColor = rd16(payload + 1);
            ui_bgChanged();
            break;

        default:
            break;
    }
}

// ----------------------------------------------------------------------------
//  Приём из USB CDC (конечный автомат)
// ----------------------------------------------------------------------------
enum RxState : uint8_t { RX_H1, RX_H2, RX_CMD, RX_LEN_H, RX_LEN_L, RX_PAYLOAD, RX_CRC };

static RxState  rxState = RX_H1;
static uint8_t  rxCmd = 0;
static uint16_t rxLen = 0, rxPos = 0;
static uint32_t rxLastByteMs = 0;
static uint8_t  rxBuf[MAX_PAYLOAD];

static void pollSerial() {
    // таймаут: если пакет оборвался на середине, начинаем поиск заголовка заново
    if (rxState != RX_H1 && millis() - rxLastByteMs > 500) rxState = RX_H1;

    int avail;
    while ((avail = Serial.available()) > 0) {
        rxLastByteMs = millis();

        if (rxState == RX_PAYLOAD) {                    // payload читаем блоком
            size_t need = rxLen - rxPos;
            size_t n = (size_t)avail < need ? (size_t)avail : need;
            n = Serial.readBytes((char *)rxBuf + rxPos, n);
            rxPos += n;
            if (rxPos >= rxLen) rxState = RX_CRC;
            continue;
        }

        uint8_t b = (uint8_t)Serial.read();
        switch (rxState) {
            case RX_H1:
                if (b == PKT_H1) rxState = RX_H2;
                break;
            case RX_H2:
                if (b == PKT_H2)      rxState = RX_CMD;
                else if (b != PKT_H1) rxState = RX_H1;  // 0xAA 0xAA 0xBB тоже валиден
                break;
            case RX_CMD:
                rxCmd = b;
                rxState = RX_LEN_H;
                break;
            case RX_LEN_H:
                rxLen = (uint16_t)b << 8;
                rxState = RX_LEN_L;
                break;
            case RX_LEN_L:
                rxLen |= b;
                rxPos = 0;
                if (rxLen > MAX_PAYLOAD) rxState = RX_H1;          // слишком длинный: мусор
                else rxState = rxLen ? RX_PAYLOAD : RX_CRC;
                break;
            case RX_CRC:
                if (packetCrc(rxCmd, rxLen, rxBuf) == b) processPacket(rxCmd, rxBuf, rxLen);
                rxState = RX_H1;
                break;
            default:
                rxState = RX_H1;
                break;
        }
    }
}

// при (пере)подключении ПК просим прислать состояние заново.
// ПК в ответ на CMD_CONFIGURATION должен отправить CREATE для всех сессий.
// ПК-программа закрыла порт: забываем всё и показываем «Сервер не запущен»
static void onServerDisconnected() {
    for (auto &s : sessions) {
        s.AppName[0] = '\0';
        s.hasIcon = false;
        s.bgColor = 0;
    }
    selectedId = -1;
    cur = CurrentTrack{};
    rxState = RX_H1;                    // оборвать недополученный пакет
    serverUp = false;
    ui_selectionChanged();
    ui_serverStateChanged();
}

static void checkConnection() {
    static bool was = false;
    bool now = (bool)Serial;            // true, пока программа на ПК держит порт открытым (DTR)
    if (now && !was) {
        serverUp = true;
        ui_serverStateChanged();
        sendPacket(CMD_CONFIGURATION, nullptr, 0);
    } else if (!now && was) {
        onServerDisconnected();
    }
    was = now;
}

// ----------------------------------------------------------------------------
//  Кнопки
// ----------------------------------------------------------------------------
struct Button {
    uint8_t  pin;
    bool     stable;
    bool     lastRaw;
    uint32_t t;
};
static Button btnUp   = { BTN_UP_PIN,   true, true, 0 };
static Button btnDown = { BTN_DOWN_PIN, true, true, 0 };

static bool buttonPressed(Button &b) {                  // true один раз за нажатие
    bool raw = digitalRead(b.pin);
    if (raw != b.lastRaw) { b.lastRaw = raw; b.t = millis(); }
    if (millis() - b.t > 30 && raw != b.stable) {
        b.stable = raw;
        if (!raw) return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
//  Локальное досчитывание времени (между пакетами от ПК)
// ----------------------------------------------------------------------------
static void trackTimeTick() {
    uint32_t now = millis();
    if (now - lastTickMs < 1000) return;
    lastTickMs = now;
    if (selectedId >= 0 && cur.state == 1 && cur.durationSec > 0 && cur.timeSec < cur.durationSec) {
        cur.timeSec++;
        ui_timeChanged();
    }
}

// ----------------------------------------------------------------------------
//  LVGL <-> LovyanGFX
// ----------------------------------------------------------------------------
static uint8_t drawBuf1[SCREEN_W * 24 * 2] __attribute__((aligned(4)));
static uint8_t drawBuf2[SCREEN_W * 24 * 2] __attribute__((aligned(4)));

static void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels((lgfx::rgb565_t *)px_map, w * h);
    lcd.endWrite();
    lv_display_flush_ready(disp);
}

// Передаёт LVGL, сколько миллисекунд прошло с прошлого вызова.
// Без этого lv_timer_handler() не обновляет экран. Вызывать в каждой итерации loop().
static void LVGL_ticker() {
    static uint32_t last = 0;
    uint32_t now = millis();
    uint32_t elapsed = now - last;
    if (elapsed) {
        lv_tick_inc(elapsed);
        last = now;
    }
}

// ----------------------------------------------------------------------------
//  setup / loop
// ----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);               // для USB CDC скорость не имеет значения
    crcInit();

    pinMode(BTN_UP_PIN, INPUT_PULLUP);
    pinMode(BTN_DOWN_PIN, INPUT_PULLUP);

    lcd.init();
    lcd.setRotation(1);                 // 320x240 альбомная
    lcd.fillScreen(TFT_BLACK);

    lv_init();

    lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flushCb);
    lv_display_set_buffers(disp, drawBuf1, drawBuf2, sizeof(drawBuf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    ui_init();
    ui_selectionChanged();              // пустое состояние: «Нет активных приложений»
    ui_serverStateChanged();            // пока ПК не подключился: «Сервер не запущен»
}

void loop() {
    pollSerial();
    checkConnection();

    if (buttonPressed(btnUp))   selectRelative(-1);
    if (buttonPressed(btnDown)) selectRelative(+1);

    trackTimeTick();
    LVGL_ticker();
    lv_timer_handler();
}
