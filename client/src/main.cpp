#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>

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

LV_FONT_DECLARE(my_font_16);

static lv_obj_t *LabelTrackTitle = nullptr;
static lv_obj_t *LabelTrackArtist = nullptr;
static lv_obj_t *LabelTrackAlbum = nullptr;
static lv_obj_t *LabelTrackApp = nullptr;
static lv_obj_t *LabelTrackTime = nullptr;
static lv_obj_t *LabelTrackDuration = nullptr;
static lv_obj_t *LabelTrackState = nullptr;

char TrackTitle[128] = "";
char TrackArtist[128] = "";
char TrackAlbum[128] = "";
char TrackApp[128] = "";
char TrackTime[128] = "";
char TrackDuration[128] = "";
char TrackState[8] = ""; // 0 - Stopped, 1 - Playing, 2 - Paused, 3 - error
static uint8_t cmd_id = 0;
static uint16_t payload_len = 0;
static uint16_t payload_bytes_read = 0;
static uint8_t payload_buf[512] = {}; // Буфер под полезную нагрузку
unsigned char info = 0x00;

enum CommandID : uint8_t {
    CMD_TEXT_LABEL_1 = 0x01, // Название
    CMD_TEXT_LABEL_2 = 0x02, // Исполнитель
    CMD_TEXT_LABEL_3 = 0x03, // Альбом
    CMD_TEXT_LABEL_4 = 0x04, // Приложение-хост медиасессии
    CMD_TRACK_TIME = 0x05, // Время трека
    CMD_TRACK_DURATION = 0x06, // Продолжительность трека
    CMD_TRACK_STATE = 0x07, // Состояние трека
};

enum ParseState {
    WAIT_HEADER_1,  // Ожидание первого байта заголовка (0xAA)
    WAIT_HEADER_2,  // Ожидание второго байта заголовка (0xBB)
    WAIT_CMD_ID,    // Ожидание типа команды
    WAIT_LEN_HIGH,  // Ожидание старшего байта длины payload (Len >> 8)
    WAIT_LEN_LOW,   // Ожидание младшего байта длины payload (Len & 0xFF)
    READ_PAYLOAD,   // Чтение байт полезной нагрузки
    CHECK_CRC       // Проверка контрольной суммы (CRC8)
};

static ParseState state = WAIT_HEADER_1;

uint8_t calculate_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

uint16_t copy_len = 0;

void processPacket(uint8_t cmd, const uint8_t *payload, uint16_t length) {
    // Serial.println("ProcessPacket");
    switch (cmd) {
        case CMD_TEXT_LABEL_1:
            copy_len = (length < sizeof(TrackTitle) - 1) ? length : (sizeof(TrackTitle) - 1);

            // 2. Копируем байты из payload в наш буфер
            memcpy(TrackTitle, payload, copy_len);

            // 3. Обязательно ставим нуль-терминатор '\0' в конце, 
            // так как в бинарном payload его может не быть, а Си-строки требуют его для корректного вывода
            TrackTitle[copy_len] = '\0';
            // lv_label_set_text(LabelTrackTitle, TrackTitle);
            delay(10);
            // Serial.print("Data: ");
            // Serial.println(TrackTitle);
            break;
        case CMD_TEXT_LABEL_2:
            copy_len = (length < sizeof(TrackArtist) - 1) ? length : (sizeof(TrackArtist) - 1);

            // 2. Копируем байты из payload в наш буфер
            memcpy(TrackArtist, payload, copy_len);

            // 3. Обязательно ставим нуль-терминатор '\0' в конце, 
            // так как в бинарном payload его может не быть, а Си-строки требуют его для корректного вывода
            TrackArtist[copy_len] = '\0';
            // lv_label_set_text(LabelTrackArtist, TrackArtist);
            break;
        case CMD_TEXT_LABEL_3:
            copy_len = (length < sizeof(TrackAlbum) - 1) ? length : (sizeof(TrackAlbum) - 1);

            // 2. Копируем байты из payload в наш буфер
            memcpy(TrackAlbum, payload, copy_len);

            // 3. Обязательно ставим нуль-терминатор '\0' в конце, 
            // так как в бинарном payload его может не быть, а Си-строки требуют его для корректного вывода
            TrackAlbum[copy_len] = '\0';
            // lv_label_set_text(LabelTrackAlbum, TrackAlbum);
            break;
        case CMD_TEXT_LABEL_4:
            copy_len = (length < sizeof(TrackApp) - 1) ? length : (sizeof(TrackApp) - 1);

            // 2. Копируем байты из payload в наш буфер
            memcpy(TrackApp, payload, copy_len);

            // 3. Обязательно ставим нуль-терминатор '\0' в конце, 
            // так как в бинарном payload его может не быть, а Си-строки требуют его для корректного вывода
            TrackApp[copy_len] = '\0';
            // lv_label_set_text(LabelTrackApp, TrackApp);
            break;
        case CMD_TRACK_TIME:
            copy_len = (length < sizeof(TrackTime) - 1) ? length : (sizeof(TrackTime) - 1);

            // 2. Копируем байты из payload в наш буфер
            memcpy(TrackTime, payload, copy_len);

            // 3. Обязательно ставим нуль-терминатор '\0' в конце, 
            // так как в бинарном payload его может не быть, а Си-строки требуют его для корректного вывода
            TrackTime[copy_len] = '\0';
            // lv_label_set_text(LabelTrackTime, TrackTime);
            break;
        case CMD_TRACK_DURATION:
            copy_len = (length < sizeof(TrackDuration) - 1) ? length : (sizeof(TrackDuration) - 1);

            // 2. Копируем байты из payload в наш буфер
            memcpy(TrackDuration, payload, copy_len);

            // 3. Обязательно ставим нуль-терминатор '\0' в конце, 
            // так как в бинарном payload его может не быть, а Си-строки требуют его для корректного вывода
            TrackDuration[copy_len] = '\0';
            // lv_label_set_text(LabelTrackDuration, TrackDuration);
            break;
        case CMD_TRACK_STATE:
            copy_len = (length < sizeof(TrackState) - 1) ? length : (sizeof(TrackState) - 1);
            memcpy(TrackState, payload, copy_len);
            TrackState[copy_len] = '\0'; // Обязательный нуль-терминатор

            // Заменяем значения с помощью двойных кавычек и функции strcpy
            if (TrackState[0] == '0') {
                strcpy(TrackState, "[]");
            } else if (TrackState[0] == '1') {
                strcpy(TrackState, "|>");
            } else if (TrackState[0] == '2') {
                strcpy(TrackState, "||"); // Теперь размер массива позволяет это сделать безопасно
            } else if (TrackState[0] == '3') {
                strcpy(TrackState, "??");
            }
            break;
        default:
            Serial.print("Unknown command ID");
            break;
    }
}

static uint32_t last_byte_time = 0;
const uint32_t SERIAL_TIMEOUT_MS = 200; // Таймаут сброса при зависании пакета

void pollSerialBuffer() { 
    // Сброс по таймауту, если зависли в каком-то состоянии чтения
    if (state != WAIT_HEADER_1 && (millis() - last_byte_time > SERIAL_TIMEOUT_MS)) {
        state = WAIT_HEADER_1;
        // Serial.println("Sync Timeout Reset");
    }

    while (Serial.available() > 0) {
        last_byte_time = millis(); // Обновляем время при получении байта
        info = Serial.read();
        
        switch (state) {
            case WAIT_HEADER_1:
                if (info == 0xAA) {
                    state = WAIT_HEADER_2;
                }
                break;

            case WAIT_HEADER_2:
                if (info == 0xBB) {
                    state = WAIT_CMD_ID;
                } else {
                    state = WAIT_HEADER_1;
                }
                break;

            case WAIT_CMD_ID:
                cmd_id = info;
                state = WAIT_LEN_HIGH;
                break;

            case WAIT_LEN_HIGH:
                payload_len = ((uint16_t)info) << 8;
                state = WAIT_LEN_LOW;
                break;

            case WAIT_LEN_LOW:
                payload_len |= info;
                payload_bytes_read = 0;
                
                if (payload_len == 0) {
                    state = CHECK_CRC;
                } else if (payload_len > sizeof(payload_buf)) {
                    state = WAIT_HEADER_1; 
                } else {
                    state = READ_PAYLOAD;
                }
                break;

            case READ_PAYLOAD:
                payload_buf[payload_bytes_read++] = info;
                if (payload_bytes_read >= payload_len) {
                    state = CHECK_CRC;
                }
                break;

            case CHECK_CRC: {
                uint8_t calculated_crc = calculate_crc8(payload_buf, payload_len);
                if (info == calculated_crc) {
                    processPacket(cmd_id, payload_buf, payload_len);
                } else {
                    // Serial.println("CRCError");
                }
                state = WAIT_HEADER_1;
                break;
            }
            default:
                state = WAIT_HEADER_1;
                break;
        }
    }
}


#define DRAW_BUF_SIZE (240 * 20) 
uint8_t draw_buf[DRAW_BUF_SIZE * sizeof(lv_color16_t)];

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels((uint16_t *)px_map, w * h, true); 
    lcd.endWrite();

    lv_display_flush_ready(disp); 
}


static void LVGL_text_updater(lv_timer_t *timer) {
    
    // Проверка в Serial-порту
    // Serial.print(".");
    lv_label_set_text(LabelTrackTitle, TrackTitle);
    lv_label_set_text(LabelTrackArtist, TrackArtist);
    lv_label_set_text(LabelTrackAlbum, TrackAlbum);
    lv_label_set_text(LabelTrackApp, TrackApp);
    lv_label_set_text(LabelTrackTime, TrackTime);
    lv_label_set_text(LabelTrackDuration, TrackDuration);
    lv_label_set_text(LabelTrackState, TrackState);
    //lv_label_set_text_fmt(label, "Counter: %d\nText updated successfully!", counter);
}

void setup() {
    Serial.begin(115200);
    while(!Serial && millis() < 3000);
    Serial.println("Starting setup...");
    lcd.init();
    lcd.setRotation(1); 

    lv_init(); 

    lv_display_t *disp = lv_display_create(320, 240); 
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // lv_obj_t *label = lv_label_create(lv_screen_active());
    // lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    // lv_obj_set_width(label, 220); 
    // lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, 0);
    // lv_obj_set_style_text_font(label, &my_font_16, 0);
    // lv_label_set_text(label, "Hello World!\nПривет, Мир!"); 

    LabelTrackTitle = lv_label_create(lv_screen_active());
    LabelTrackArtist = lv_label_create(lv_screen_active());
    LabelTrackAlbum = lv_label_create(lv_screen_active());
    LabelTrackApp = lv_label_create(lv_screen_active());
    LabelTrackTime = lv_label_create(lv_screen_active());
    LabelTrackDuration = lv_label_create(lv_screen_active());
    LabelTrackState = lv_label_create(lv_screen_active());
    lv_label_set_long_mode(LabelTrackTitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LabelTrackTitle, 320); 
    lv_obj_align(LabelTrackTitle, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_set_style_text_font(LabelTrackTitle, &my_font_16, 0);
    lv_label_set_long_mode(LabelTrackArtist, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LabelTrackArtist, 320); 
    lv_obj_align(LabelTrackArtist, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_text_font(LabelTrackArtist, &my_font_16, 0);
    lv_label_set_long_mode(LabelTrackAlbum, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LabelTrackAlbum, 320); 
    lv_obj_align(LabelTrackAlbum, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_text_font(LabelTrackAlbum, &my_font_16, 0);
    lv_label_set_long_mode(LabelTrackApp, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LabelTrackApp, 320); 
    lv_obj_align(LabelTrackApp, LV_ALIGN_TOP_LEFT, 0, 80);
    lv_obj_set_style_text_font(LabelTrackApp, &my_font_16, 0);
    lv_label_set_long_mode(LabelTrackTime, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LabelTrackTime, 320); 
    lv_obj_align(LabelTrackTime, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_obj_set_style_text_font(LabelTrackTime, &my_font_16, 0);
    lv_label_set_long_mode(LabelTrackDuration, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LabelTrackDuration, 320); 
    lv_obj_align(LabelTrackDuration, LV_ALIGN_TOP_LEFT, 0, 120);
    lv_obj_set_style_text_font(LabelTrackDuration, &my_font_16, 0);
    lv_label_set_long_mode(LabelTrackState, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LabelTrackState, 320); 
    lv_obj_align(LabelTrackState, LV_ALIGN_TOP_LEFT, 0, 140);
    lv_obj_set_style_text_font(LabelTrackState, &my_font_16, 0);
    
    lv_label_set_text(LabelTrackTitle, "Initialization...");
    lv_label_set_text(LabelTrackArtist, "Initialization...");
    lv_label_set_text(LabelTrackAlbum, "Initialization...");
    lv_label_set_text(LabelTrackApp, "Initialization...");
    lv_label_set_text(LabelTrackTime, "Initialization...");
    lv_label_set_text(LabelTrackDuration, "Initialization...");
    lv_label_set_text(LabelTrackState, "Initialization...");

    lv_timer_create(LVGL_text_updater, 10, NULL);
    Serial.println("void setup complete");

}

uint32_t last_tick = 0;

void LVGL_ticker() { // Тикер
    // ВАЖНО: передаем прошедшее время в LVGL, чтобы таймеры начали работать
    // uint32_t current_ms = millis();
    lv_tick_inc(millis() - last_tick);
    last_tick = millis();
    lv_timer_handler(); 
}

void loop() {
    // Главный таймер LVGL
    //lv_timer_handler();
    LVGL_ticker();
    // Небольшая пауза для корректной работы системы
    delay(10);
    pollSerialBuffer();
}