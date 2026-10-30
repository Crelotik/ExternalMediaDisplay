// ============================================================================
//  media_server.cpp  —  серверная часть для дисплея «Внешний дисплей»
//
//  Читает медиасессии Windows (GlobalSystemMediaTransportControls), ведёт
//  таблицу «приложение -> слот 0..15» и общается с RP2040 по USB CDC.
//
//  Сборка (x64 Native Tools Command Prompt for VS, Windows 10 SDK >= 17763):
//      rc /nologo app.rc
//      cl /std:c++20 /EHsc /utf-8 /permissive- media_server.cpp app.res
//  Ключ /utf-8 обязателен (кириллица в надписях и журнале), файл должен быть сохранён в UTF-8.
//  app.rc встраивает значок icons\app.ico в exe; без него программа берёт icons\app.ico с диска.
//  Запуск:
//      media_server.exe [COM8] [--tray] [--dump-thumbs]
//        --tray          стартовать скрытым: только значок в трее
//        --dump-thumbs   сохранять исходные миниатюры в папку thumbs (для настройки обрезки)
//  Окно журнала: крестик сворачивает программу в трей; левый щелчок по значку
//  открывает окно, правый показывает меню («Показать окно», «Выход»).
//  Значок окна и трея: из ресурсов exe (app.rc), иначе из файла icons\app.ico. Консоль не нужна: подсистема Windows.
//  Окно: выбор порта, список медиасессий с ползунком громкости, галочка обрезки
//  обложки Spotify, состояние дисплея. Настройки хранятся в settings.ini рядом с exe.
//  Громкость приложений: команды 0x10..0x12, управление треком: команда 0x0F
//  [id][действие: 0 играть/пауза, 1 следующий, 2 предыдущий, 3 играть, 4 пауза, 5 стоп]
//  (клиентская часть пока их не использует).
//
//  Иконки приложений: PNG-файлы в папке icons рядом с exe (см. loadAppIcon).
//
//  Протокол: 0xAA 0xBB, cmd, len hi, len lo, payload, CRC8
//  CRC8: полином 0x07, init 0, по cmd + len(hi, lo) + payload (как в прошивке).
//  Если у вас своя реализация CRC8 — подгоните packetCrc() под неё.
// ============================================================================
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <appmodel.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "shell32")
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "ole32")
// без консоли: окно журнала и значок в трее создаёт сама программа
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup")
#pragma comment(lib, "comctl32")
// современный вид стандартных элементов (список, ползунок, галочка)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Кириллица в строках: файл должен быть сохранён в UTF-8 и собираться с ключом /utf-8, иначе
// надписи в окне и журнал превращаются в «кракозябры». Проверяем это прямо при сборке.
static_assert((unsigned char)"я"[0] == 0xD1 && (unsigned char)"я"[1] == 0x8F && L"я"[0] == 0x044F,
    "Build with /utf-8 and keep this file saved as UTF-8 (see the comment at the top of the file).");

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage::Streams;

using Manager = GlobalSystemMediaTransportControlsSessionManager;
using Session = GlobalSystemMediaTransportControlsSession;
using PStatus = GlobalSystemMediaTransportControlsSessionPlaybackStatus;

// ----------------------------------------------------------------------------
//  Константы и команды (те же, что в прошивке)
// ----------------------------------------------------------------------------
constexpr uint8_t  MAX_SESSIONS = 16;
constexpr uint32_t COVER_SIZE = 128;
constexpr uint32_t ICON_SIZE = 32;    // как ICON_SIZE в прошивке

enum Cmd : uint8_t {
    CMD_CONFIGURATION = 0x01,
    CMD_CREATE_MEDIASESSION = 0x02,
    CMD_DELETE_MEDIASESSION = 0x03,
    CMD_SELECT_MEDIASESSION = 0x04,
    CMD_TRACK_NAME = 0x05,
    CMD_TRACK_ARTIST = 0x06,
    CMD_TRACK_ALBUM = 0x07,
    CMD_TRACK_CURRENT_TIME = 0x08,
    CMD_TRACK_DURATION = 0x09,
    CMD_TRACK_STATE = 0x0A,
    CMD_TRACK_PHOTO = 0x0B,
    CMD_APP_ICON = 0x0C,   // иконка 32x32 из icons\<ключ>.png (если файл есть)
    CMD_APPS_BACKGROUND_COLOR = 0x0D,
    CMD_TRACK_BACKGROUND_COLOR = 0x0E,
    CMD_CONTROL = 0x0F,   // МК -> ПК: [id][действие], см. enum Ctrl
    CMD_VOLUME_STEP = 0x10,   // МК -> ПК: [id][int8 шаг в процентах, например +5 / -5]
    CMD_VOLUME_MUTE = 0x11,   // МК -> ПК: [id][0 включить звук, 1 выключить, 2 переключить]
    CMD_VOLUME_STATE = 0x12,   // ПК -> МК: [id][громкость 0..100][mute 0/1][доступна 0/1]
};

// действия CMD_CONTROL
enum Ctrl : uint8_t {
    CTRL_PLAY_PAUSE = 0,   // переключить воспроизведение / паузу
    CTRL_NEXT = 1,   // следующий трек
    CTRL_PREV = 2,   // предыдущий трек
    CTRL_PLAY = 3,
    CTRL_PAUSE = 4,
    CTRL_STOP = 5,
};

// ----------------------------------------------------------------------------
//  Утилиты
// ----------------------------------------------------------------------------
static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string u8(hstring const& h) { return toUtf8(std::wstring(h.c_str(), h.size())); }

// обрезка UTF-8 строки по границе символа
static std::string cutUtf8(std::string s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) n--;
    s.resize(n);
    return s;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// стабильный «приятный» цвет из строки (для цвета пункта в списке приложений)
static uint16_t colorFromString(const std::wstring& s) {
    uint32_t hsh = 2166136261u;
    for (wchar_t c : s) { hsh ^= (uint32_t)c; hsh *= 16777619u; }
    float h = (hsh % 360) / 60.0f, S = 0.55f, V = 0.45f;
    float C = V * S, X = C * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f)), m = V - C;
    float r = 0, g = 0, b = 0;
    switch ((int)h) {
    case 0: r = C; g = X; break;
    case 1: r = X; g = C; break;
    case 2: g = C; b = X; break;
    case 3: g = X; b = C; break;
    case 4: r = X; b = C; break;
    default: r = C; b = X; break;
    }
    return rgb565((uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255), (uint8_t)((b + m) * 255));
}

// Известные приложения: ключ (подстрока AppUserModelId, строчными) -> отображаемое имя.
// Ключ одновременно служит именем файла иконки: icons\<ключ>.png
struct KnownApp { const wchar_t* key; const wchar_t* name; };
static const KnownApp kKnownApps[] = {
    { L"spotify",      L"Spotify" },
    { L"yandexmusic",  L"Яндекс Музыка" },
    { L"yandex.music", L"Яндекс Музыка" },
    { L"msedge",       L"Edge" },
    { L"chrome",       L"Chrome" },
    { L"firefox",      L"Firefox" },
    { L"opera",        L"Opera" },
    { L"vlc",          L"VLC" },
    { L"zunemusic",    L"Медиаплеер" },
    { L"aimp",         L"AIMP" },
    { L"foobar",       L"foobar2000" },
};

struct AppInfo {
    std::string  name;      // отображаемое имя (UTF-8)
    std::wstring iconKey;   // имя файла иконки без расширения
};

static std::wstring lowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static AppInfo lookupApp(const std::wstring& id) {
    std::wstring l = lowerW(id);
    for (auto& kv : kKnownApps)
        if (l.find(kv.key) != std::wstring::npos) return { toUtf8(kv.name), kv.key };

    // запасной вариант: часть после '!', без пути и ".exe"
    std::wstring n = id;
    size_t bang = n.rfind(L'!');
    if (bang != std::wstring::npos) n = n.substr(bang + 1);
    size_t slash = n.find_last_of(L"\\/");
    if (slash != std::wstring::npos) n = n.substr(slash + 1);
    if (n.size() > 4 && lowerW(n.substr(n.size() - 4)) == L".exe") n.resize(n.size() - 4);

    std::wstring key;                                  // безопасное для файла имя
    for (wchar_t c : lowerW(n))
        if (iswalnum(c) || c == L'.' || c == L'-' || c == L'_') key += c;

    if (!n.empty()) n[0] = (wchar_t)towupper(n[0]);
    return { n.empty() ? std::string("App") : toUtf8(n), key };
}

// ----------------------------------------------------------------------------
//  CRC8
// ----------------------------------------------------------------------------
static uint8_t g_crcTable[256];

static void crcInit() {
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)i;
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
        g_crcTable[i] = c;
    }
}

static uint8_t packetCrc(uint8_t cmd, uint16_t len, const uint8_t* payload) {
    uint8_t c = 0;
    c = g_crcTable[c ^ cmd];
    c = g_crcTable[c ^ (uint8_t)(len >> 8)];
    c = g_crcTable[c ^ (uint8_t)(len & 0xFF)];
    for (uint16_t i = 0; i < len; i++) c = g_crcTable[c ^ payload[i]];
    return c;
}

// ----------------------------------------------------------------------------
//  Последовательный порт (USB CDC), один поток: чтение без блокировки
// ----------------------------------------------------------------------------
class SerialPort {
public:
    bool isOpen() const { return h_ != INVALID_HANDLE_VALUE; }

    // Открытие порта может надолго «зависнуть» (например, на Bluetooth-порту), поэтому оно
    // вынесено в статическую функцию и выполняется в фоновом потоке (см. startOpenJob).
    static HANDLE openHandle(const std::wstring& name) {
        std::wstring path = L"\\\\.\\" + name;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h, &dcb)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
        dcb.BaudRate = CBR_115200;         // для USB CDC значение не важно
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE; // МК определяет подключение по DTR
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutX = dcb.fInX = FALSE;
        if (!SetCommState(h, &dcb)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }

        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;   // ReadFile возвращает сразу то, что есть
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant = 0;
        to.WriteTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant = 1000;       // неотвечающее устройство не должно подолгу вешать программу
        SetCommTimeouts(h, &to);
        SetupComm(h, 1 << 16, 1 << 16);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        EscapeCommFunction(h, SETDTR);
        return h;
    }

    void attach(HANDLE h) {                          // принять готовый (уже настроенный) порт
        close();
        h_ = h;
    }

    void close() {
        if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    }

    bool write(const uint8_t* d, size_t n) {
        if (!isOpen()) return false;
        size_t off = 0;
        while (off < n) {
            DWORD w = 0;
            if (!WriteFile(h_, d + off, (DWORD)(n - off), &w, nullptr) || w == 0) { close(); return false; }
            off += w;
        }
        return true;
    }

    size_t read(uint8_t* buf, size_t cap) {
        if (!isOpen()) return 0;
        DWORD got = 0;
        if (!ReadFile(h_, buf, (DWORD)cap, &got, nullptr)) { close(); return 0; }
        return got;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

static SerialPort g_port;

// ----------------------------------------------------------------------------
//  Отправка пакетов
// ----------------------------------------------------------------------------
static bool sendPacket(uint8_t cmd, const uint8_t* p, size_t n) {
    if (!g_port.isOpen() || n > 0xFFFF) return false;
    std::vector<uint8_t> f;
    f.reserve(n + 6);
    f.push_back(0xAA);
    f.push_back(0xBB);
    f.push_back(cmd);
    f.push_back((uint8_t)(n >> 8));
    f.push_back((uint8_t)(n & 0xFF));
    if (n) f.insert(f.end(), p, p + n);
    f.push_back(packetCrc(cmd, (uint16_t)n, p));
    return g_port.write(f.data(), f.size());
}

static bool sendStr(uint8_t cmd, uint8_t id, const std::string& s) {
    std::vector<uint8_t> b;
    b.reserve(1 + s.size());
    b.push_back(id);
    b.insert(b.end(), s.begin(), s.end());
    return sendPacket(cmd, b.data(), b.size());
}

static bool sendU32(uint8_t cmd, uint8_t id, uint32_t v) {
    uint8_t b[5] = { id, (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return sendPacket(cmd, b, sizeof(b));
}

static bool sendU8(uint8_t cmd, uint8_t id, uint8_t v) {
    uint8_t b[2] = { id, v };
    return sendPacket(cmd, b, sizeof(b));
}

static bool sendColor(uint8_t cmd, uint8_t id, uint16_t c) {
    uint8_t b[3] = { id, (uint8_t)(c >> 8), (uint8_t)(c & 0xFF) };
    return sendPacket(cmd, b, sizeof(b));
}

// [id][w hi][w lo][h hi][h lo][format=0][RGB565 big-endian ...]
static bool sendImage(uint8_t cmd, uint8_t id, const std::vector<uint8_t>& px, uint16_t w, uint16_t h) {
    std::vector<uint8_t> b;
    b.reserve(6 + px.size());
    b.push_back(id);
    b.push_back((uint8_t)(w >> 8)); b.push_back((uint8_t)(w & 0xFF));
    b.push_back((uint8_t)(h >> 8)); b.push_back((uint8_t)(h & 0xFF));
    b.push_back(0);
    b.insert(b.end(), px.begin(), px.end());
    return sendPacket(cmd, b.data(), b.size());
}

// ----------------------------------------------------------------------------
//  Приём пакетов от МК (CONFIGURATION, SELECT)
// ----------------------------------------------------------------------------
struct Incoming { uint8_t cmd; std::vector<uint8_t> payload; };

class RxParser {
public:
    std::deque<Incoming> out;

    void reset() { st_ = H1; out.clear(); }

    void feed(uint8_t b) {
        switch (st_) {
        case H1:  if (b == 0xAA) st_ = H2; break;
        case H2:  if (b == 0xBB) st_ = CMD; else if (b != 0xAA) st_ = H1; break;
        case CMD: cmd_ = b; st_ = LH; break;
        case LH:  len_ = (uint16_t)(b << 8); st_ = LL; break;
        case LL:
            len_ |= b;
            pay_.clear();
            if (len_ > 64) st_ = H1;            // от МК приходят только короткие пакеты
            else st_ = len_ ? PAY : CRC;
            break;
        case PAY:
            pay_.push_back(b);
            if (pay_.size() >= len_) st_ = CRC;
            break;
        case CRC:
            if (packetCrc(cmd_, len_, pay_.data()) == b) out.push_back({ cmd_, pay_ });
            st_ = H1;
            break;
        }
    }

private:
    enum St { H1, H2, CMD, LH, LL, PAY, CRC } st_ = H1;
    uint8_t cmd_ = 0;
    uint16_t len_ = 0;
    std::vector<uint8_t> pay_;
};

static RxParser g_parser;

// ----------------------------------------------------------------------------
//  Состояние: слоты приложений и выбранный трек
// ----------------------------------------------------------------------------
struct Slot {
    bool used = false;
    bool known = false;                 // МК получил CREATE для этого слота
    std::wstring appId;
    std::string name;
    uint16_t color = 0;
    std::wstring iconKey;               // ключ из kKnownApps / имя файла иконки (также для поиска звука)
    std::vector<uint8_t> icon;          // RGB565 big-endian 32x32 (пусто = иконки нет)
    Session session{ nullptr };
};
static std::array<Slot, MAX_SESSIONS> g_slots;
static int g_selected = -1;             // слот, выбранный на МК

enum class CoverState { Pending, Ready, None };

struct Track {                          // что известно ПК о выбранной сессии
    std::wstring key;                   // title|artist|album: признак смены трека
    std::string title, artist, album;
    uint32_t dur = 0, time = 0;
    uint8_t  state = 0;                 // 0 stop, 1 play, 2 pause
    CoverState cs = CoverState::Pending;
    std::vector<uint8_t> cover;         // RGB565 big-endian 128x128
    uint16_t color = 0;                 // цвет фона по обложке (0 = не задан)
    int tries = 0;
    ULONGLONG nextTry = 0;
    uint32_t coverGen = 0;              // растёт, когда решение по обложке принято
};
static Track g_cur;

struct Sent {                           // что уже отправлено на МК
    std::optional<std::string> title, artist, album;
    std::optional<uint32_t> dur, time;
    std::optional<uint8_t>  state;
    std::optional<uint32_t> vol;        // громкость: доступна<<16 | уровень<<8 | mute
    uint32_t coverGen = 0;
};
static Sent g_sent;

static bool g_needResync = false;
static ULONGLONG g_lastResync = 0;

// ----------------------------------------------------------------------------
//  Обложка: уменьшить до 128x128 (с обрезкой по центру) и перевести в RGB565
// ----------------------------------------------------------------------------
// Обрезка исходной миниатюры ДО масштабирования (доли от размера исходного изображения).
// Spotify вставляет в миниатюру собственную полосу с названием снизу и обрезает обложку.
// Подберите SPOTIFY_CROP_BOTTOM: запустите программу с ключом --dump-thumbs, откройте
// файл thumbs\spotify.jpg (или .png) и поделите высоту лишней полосы на высоту картинки.
constexpr double SPOTIFY_CROP_BOTTOM = 0.22;
static bool g_spotifyCrop = true;                  // галочка в окне программы

struct CropRule { double top = 0, bottom = 0, left = 0, right = 0; };

static CropRule cropFor(const std::wstring& iconKey) {
    CropRule r;
    if (iconKey == L"spotify" && g_spotifyCrop) r.bottom = SPOTIFY_CROP_BOTTOM;   // только для Spotify
    return r;
}

static bool makeCover(IRandomAccessStreamReference const& thumb, std::vector<uint8_t>& out,
    uint16_t& avgColor, const CropRule& crop = CropRule{}) {
    try {
        auto stream = thumb.OpenReadAsync().get();
        auto decoder = BitmapDecoder::CreateAsync(stream).get();

        const int64_t w = decoder.PixelWidth(), h = decoder.PixelHeight();
        if (w <= 0 || h <= 0) return false;

        // 1) область исходника после обрезки (в пикселях исходного изображения)
        int64_t cx = std::llround(w * crop.left);
        int64_t cy = std::llround(h * crop.top);
        int64_t cw = w - cx - std::llround(w * crop.right);
        int64_t ch = h - cy - std::llround(h * crop.bottom);
        if (cx < 0 || cy < 0 || cw < 32 || ch < 32) { cx = cy = 0; cw = w; ch = h; }   // неверные значения: без обрезки

        // 2) из неё берём центральный квадрат
        const int64_t side = std::min(cw, ch);
        const int64_t sx = cx + (cw - side) / 2;
        const int64_t sy = cy + (ch - side) / 2;

        // 3) масштабируем так, чтобы этот квадрат стал 128x128, и вырезаем его
        //    (Bounds применяются ПОСЛЕ масштабирования)
        const double f = double(COVER_SIZE) / double(side);
        const int64_t sw = std::max<int64_t>(COVER_SIZE, std::llround(w * f));
        const int64_t sh = std::max<int64_t>(COVER_SIZE, std::llround(h * f));
        const int64_t bx = std::clamp<int64_t>(std::llround(sx * f), 0, sw - COVER_SIZE);
        const int64_t by = std::clamp<int64_t>(std::llround(sy * f), 0, sh - COVER_SIZE);

        BitmapTransform tr;
        tr.ScaledWidth((uint32_t)sw);
        tr.ScaledHeight((uint32_t)sh);
        tr.InterpolationMode(BitmapInterpolationMode::Fant);
        tr.Bounds(BitmapBounds{ (uint32_t)bx, (uint32_t)by, COVER_SIZE, COVER_SIZE });

        auto provider = decoder.GetPixelDataAsync(
            BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, tr,
            ExifOrientationMode::IgnoreExifOrientation,
            ColorManagementMode::DoNotColorManage).get();
        auto px = provider.DetachPixelData();
        if (px.size() < (size_t)COVER_SIZE * COVER_SIZE * 4) return false;

        out.resize((size_t)COVER_SIZE * COVER_SIZE * 2);
        uint64_t sr = 0, sg = 0, sb = 0;
        for (size_t i = 0; i < (size_t)COVER_SIZE * COVER_SIZE; i++) {
            uint8_t b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
            uint16_t c = rgb565(r, g, b);
            out[i * 2] = (uint8_t)(c >> 8);       // старший байт первым
            out[i * 2 + 1] = (uint8_t)(c & 0xFF);
            sr += r; sg += g; sb += b;
        }
        // средний цвет, затемнённый, чтобы белый текст оставался читаемым
        const double n = (double)COVER_SIZE * COVER_SIZE, k = 0.45;
        uint16_t c = rgb565((uint8_t)(sr / n * k), (uint8_t)(sg / n * k), (uint8_t)(sb / n * k));
        avgColor = c ? c : 0x0841;                    // 0 = «не задан», поэтому избегаем нуля
        return true;
    }
    catch (...) {
        return false;
    }
}

// ----------------------------------------------------------------------------
//  Иконки приложений: готовые PNG в папке icons рядом с exe
//  Имя файла = ключ из kKnownApps (spotify.png, chrome.png, ...) либо имя
//  приложения строчными буквами. Лучше квадратные, от 64x64 (масштабируются до 32x32).
// ----------------------------------------------------------------------------
static std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

// PNG -> 32x32 RGB565 (big-endian). Прозрачность смешивается с цветом пункта списка.
static std::vector<uint8_t> loadAppIcon(const std::wstring& key, uint16_t bgColor) {
    std::vector<uint8_t> out;
    if (key.empty()) return out;

    std::wstring path = exeDir() + L"\\icons\\" + key + L".png";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return out;

    try {
        auto file = Windows::Storage::StorageFile::GetFileFromPathAsync(path).get();
        auto stream = file.OpenAsync(Windows::Storage::FileAccessMode::Read).get();
        auto decoder = BitmapDecoder::CreateAsync(stream).get();

        BitmapTransform tr;
        tr.ScaledWidth(ICON_SIZE);
        tr.ScaledHeight(ICON_SIZE);
        tr.InterpolationMode(BitmapInterpolationMode::Fant);

        auto provider = decoder.GetPixelDataAsync(
            BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, tr,
            ExifOrientationMode::IgnoreExifOrientation,
            ColorManagementMode::DoNotColorManage).get();
        auto px = provider.DetachPixelData();
        if (px.size() < (size_t)ICON_SIZE * ICON_SIZE * 4) return {};

        const int bgR = ((bgColor >> 11) & 0x1F) * 255 / 31;
        const int bgG = ((bgColor >> 5) & 0x3F) * 255 / 63;
        const int bgB = (bgColor & 0x1F) * 255 / 31;

        out.resize((size_t)ICON_SIZE * ICON_SIZE * 2);
        for (size_t i = 0; i < (size_t)ICON_SIZE * ICON_SIZE; i++) {
            int a = px[i * 4 + 3];
            // цвета уже умножены на альфу: результат = цвет + фон * (1 - альфа)
            int b = std::min(255, px[i * 4 + 0] + bgB * (255 - a) / 255);
            int g = std::min(255, px[i * 4 + 1] + bgG * (255 - a) / 255);
            int r = std::min(255, px[i * 4 + 2] + bgR * (255 - a) / 255);
            uint16_t c = rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
            out[i * 2] = (uint8_t)(c >> 8);
            out[i * 2 + 1] = (uint8_t)(c & 0xFF);
        }
    }
    catch (...) {
        out.clear();
    }
    return out;
}

// ----------------------------------------------------------------------------
//  Окно журнала и значок в трее
//  Крестик окна сворачивает программу в трей. Левый щелчок по значку открывает
//  окно, правый показывает меню («Показать окно», «Выход»).
// ----------------------------------------------------------------------------
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT TRAY_ID = 1;
constexpr UINT IDM_SHOW = 1001;
constexpr UINT IDM_EXIT = 1002;
static const wchar_t* kWndClass = L"ExternalDisplayMediaServerWnd";

static std::atomic<bool> g_running{ true };
static HWND   g_hwnd = nullptr;
static HWND   g_edit = nullptr;
static HICON  g_icon = nullptr;             // большой значок (окно, панель задач)
static HICON  g_iconSmall = nullptr;        // маленький значок (заголовок окна, трей)
static HFONT  g_font = nullptr;
static HBRUSH g_bgBrush = nullptr;
static UINT   g_wmTaskbarCreated = 0;
static NOTIFYICONDATAW g_nid{};

static HWND         g_statusLbl = nullptr;       // «Дисплей: подключен / ...»
static int          g_uiStatus = -1;             // 0 порт закрыт, 1 порт открыт, ответа нет, 2 подключен
static std::wstring g_portName = L"COM8";        // выбранный порт
static bool         g_portChanged = false;       // порт выбран в списке: переподключиться сразу
static bool         g_mcuSeen = false;           // с момента открытия порта МК прислал хотя бы один пакет
static bool         g_wasOpen = false;           // порт был открыт на прошлом проходе главного цикла
static ULONGLONG    g_retryDelayMs = 1000;       // пауза между попытками подключения (растёт, если МК молчит)

// ---- открытие порта в фоновом потоке ----
struct OpenJob {
    std::wstring name;
    std::atomic<bool> done{ false };
    std::atomic<bool> cancelled{ false };
    HANDLE h = INVALID_HANDLE_VALUE;
};
static std::vector<std::shared_ptr<OpenJob>> g_openJobs;

static void startOpenJob(const std::wstring& name) {
    auto job = std::make_shared<OpenJob>();
    job->name = name;
    g_openJobs.push_back(job);
    std::thread([job] {
        job->h = SerialPort::openHandle(job->name);
        job->done = true;
        }).detach();
}

static bool hasPendingJob(const std::wstring& name) {
    for (auto& j : g_openJobs)
        if (!j->done && !j->cancelled && j->name == name) return true;
    return false;
}

static void cancelOpenJobs() {
    for (auto& j : g_openJobs) j->cancelled = true;
}

// забирает результаты завершённых попыток; true, если порт подключён
static bool collectOpenJobs() {
    bool attached = false;
    for (auto it = g_openJobs.begin(); it != g_openJobs.end();) {
        auto& j = *it;
        if (!j->done) { ++it; continue; }
        if (j->h != INVALID_HANDLE_VALUE) {
            if (!j->cancelled && !g_port.isOpen() && j->name == g_portName) { g_port.attach(j->h); attached = true; }
            else CloseHandle(j->h);                  // результат больше не нужен
        }
        it = g_openJobs.erase(it);
    }
    return attached;
}

// printf-подобный вывод в окно журнала (строки в UTF-8)
static void logLine(const char* fmt, ...) {
    if (!g_edit) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int n = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
    if (n <= 1) return;
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, w.data(), n);

    std::wstring t;                                   // EDIT понимает только \r\n
    t.reserve(w.size() + 8);
    for (wchar_t c : w) { if (c == L'\n') t += L'\r'; t += c; }

    int len = GetWindowTextLengthW(g_edit);
    if (len > 60000) {                                // журнал не должен разрастаться
        SendMessageW(g_edit, EM_SETSEL, 0, 20000);
        SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(g_edit);
    }
    SendMessageW(g_edit, EM_SETSEL, len, len);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
}

static void showMainWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);                   // если окно было свёрнуто
    SetForegroundWindow(g_hwnd);
}

constexpr int IDI_APPICON = 1;                         // идентификатор значка в ресурсах exe (см. app.rc)

// значок нужного размера: 1) из ресурсов exe, 2) из файла icons\app.ico рядом с exe, 3) стандартный
static HICON loadWindowIcon(int size) {
    HANDLE h = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    if (h) return (HICON)h;

    std::wstring path = exeDir() + L"\\icons\\app.ico";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        h = LoadImageW(nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE);
        if (h) return (HICON)h;
    }
    return LoadIconW(nullptr, IDI_APPLICATION);
}

static void trayAdd() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    wcscpy_s(g_nid.szTip, L"Внешний дисплей: сервер");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void trayRemove() {
    if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Показать окно");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Выход");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);                      // иначе меню не закроется щелчком мимо
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// определены ниже, в разделе «Интерфейс окна»
static void createControls(HINSTANCE hi);
static void layoutControls(int cw, int ch);
static void onUiCommand(int id, int code);
static void onUiVScroll(HWND from, int code);

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_wmTaskbarCreated && m == g_wmTaskbarCreated) { trayAdd(); return 0; }   // explorer перезапущен

    switch (m) {
    case WM_SIZE:
        layoutControls(LOWORD(l), HIWORD(l));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize.x = 560;
        mmi->ptMinTrackSize.y = 560;
        return 0;
    }
    case WM_CLOSE:                                // крестик: прячем в трей, программа продолжает работать
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_LBUTTONDBLCLK) showMainWindow();
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) showTrayMenu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDM_SHOW) showMainWindow();
        else if (LOWORD(w) == IDM_EXIT) g_running = false;
        else onUiCommand(LOWORD(w), HIWORD(w));
        return 0;
    case WM_VSCROLL:                              // вертикальный ползунок громкости
        onUiVScroll((HWND)l, LOWORD(w));
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        HWND ctl = (HWND)l;
        if (ctl == g_edit) {                      // журнал: тёмная консольная тема
            SetTextColor(dc, RGB(220, 220, 220));
            SetBkColor(dc, RGB(24, 24, 24));
            return (LRESULT)g_bgBrush;
        }
        SetBkMode(dc, TRANSPARENT);               // остальные подписи: фон окна
        if (ctl == g_statusLbl)
            SetTextColor(dc, g_uiStatus == 2 ? RGB(0, 140, 0) : (g_uiStatus == 1 || g_uiStatus == 3) ? RGB(200, 120, 0) : RGB(190, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_CTLCOLOREDIT:
        if ((HWND)l == g_edit) {
            SetTextColor((HDC)w, RGB(220, 220, 220));
            SetBkColor((HDC)w, RGB(24, 24, 24));
            return (LRESULT)g_bgBrush;
        }
        break;
    case WM_ENDSESSION:                           // выключение Windows
        if (w) g_running = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool createMainWindow(HINSTANCE hi, bool startHidden) {
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = loadWindowIcon(GetSystemMetrics(SM_CXICON));
    g_iconSmall = loadWindowIcon(GetSystemMetrics(SM_CXSMICON));
    g_bgBrush = CreateSolidBrush(RGB(24, 24, 24));    // фон окна журнала

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hi;
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_icon;
    wc.hIconSm = g_iconSmall;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc)) return false;

    g_hwnd = CreateWindowExW(0, kWndClass, L"Внешний дисплей: сервер", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 660, nullptr, nullptr, hi, nullptr);
    if (!g_hwnd) return false;

    createControls(hi);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    layoutControls(rc.right, rc.bottom);

    trayAdd();
    ShowWindow(g_hwnd, startHidden ? SW_HIDE : SW_SHOW);
    return true;
}

static void pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g_running = false;
        if (g_hwnd && IsDialogMessageW(g_hwnd, &msg)) continue;     // Tab между элементами окна
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ----------------------------------------------------------------------------
//  Отладка: сохранить исходную миниатюру трека (ключ --dump-thumbs)
//  Файл thumbs\<ключ приложения>.jpg/.png: по нему подбирается обрезка (cropFor)
// ----------------------------------------------------------------------------
static bool g_dumpThumbs = false;

static void dumpThumbnail(IRandomAccessStreamReference const& thumb, const std::wstring& key) {
    try {
        auto stream = thumb.OpenReadAsync().get();
        uint64_t size = stream.Size();
        if (size < 4 || size > 16 * 1024 * 1024) return;

        DataReader reader(stream);
        reader.LoadAsync((uint32_t)size).get();
        std::vector<uint8_t> bytes((size_t)size);
        reader.ReadBytes(bytes);

        const wchar_t* ext = L".bin";
        if (bytes[0] == 0xFF && bytes[1] == 0xD8)                          ext = L".jpg";
        else if (bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N')   ext = L".png";
        else if (bytes[0] == 'B' && bytes[1] == 'M')                       ext = L".bmp";

        std::wstring dir = exeDir() + L"\\thumbs";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring name = (key.empty() ? L"unknown" : key) + ext;
        HANDLE f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD wr = 0;
        WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
        CloseHandle(f);
        logLine("[d] миниатюра сохранена: thumbs\\%s\n", toUtf8(name).c_str());
    }
    catch (...) {
    }
}

// ----------------------------------------------------------------------------
//  Громкость приложения (микшер Windows, Core Audio / WASAPI)
//  Медиасессия -> аудиосессия того же приложения (по процессу). У браузеров
//  аудиосессий может быть несколько: громкость меняется у всех.
// ----------------------------------------------------------------------------
using AudioVolumes = std::vector<winrt::com_ptr<ISimpleAudioVolume>>;

// имя exe процесса (строчными, без пути и ".exe") и AppUserModelId упакованного приложения
static std::wstring processInfo(DWORD pid, std::wstring& aumid) {
    aumid.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";

    std::wstring base;
    wchar_t path[MAX_PATH * 2];
    DWORD n = (DWORD)std::size(path);
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        base.assign(path, n);
        size_t slash = base.find_last_of(L"\\/");
        if (slash != std::wstring::npos) base = base.substr(slash + 1);
        base = lowerW(base);
        if (base.size() > 4 && base.compare(base.size() - 4, 4, L".exe") == 0) base.resize(base.size() - 4);
    }
    wchar_t id[256];
    UINT32 idLen = (UINT32)std::size(id);
    if (GetApplicationUserModelId(h, &idLen, id) == ERROR_SUCCESS) aumid.assign(id);
    CloseHandle(h);
    return base;
}

static bool audioMatchesSlot(DWORD pid, const Slot& s) {
    std::wstring aumid;
    std::wstring exe = processInfo(pid, aumid);
    std::wstring id = lowerW(s.appId);

    if (!aumid.empty() && lowerW(aumid) == id) return true;               // приложение из Microsoft Store
    if (exe.empty()) return false;

    std::wstring idBase = id;                                             // "Spotify.exe" -> "spotify"
    if (idBase.size() > 4 && idBase.compare(idBase.size() - 4, 4, L".exe") == 0) idBase.resize(idBase.size() - 4);
    if (idBase == exe) return true;                                       // Spotify.exe, vlc.exe, Chrome ...
    if (s.iconKey.size() >= 4 && exe.find(s.iconKey) != std::wstring::npos) return true;   // chrome, msedge ...
    return false;
}

static AudioVolumes findAppVolumes(const Slot& s) {
    AudioVolumes out;
    try {
        auto enumr = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator), CLSCTX_ALL);
        winrt::com_ptr<IMMDevice> dev;
        if (FAILED(enumr->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.put()))) return out;

        winrt::com_ptr<IAudioSessionManager2> mgr;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, mgr.put_void()))) return out;

        winrt::com_ptr<IAudioSessionEnumerator> list;
        if (FAILED(mgr->GetSessionEnumerator(list.put()))) return out;

        int count = 0;
        list->GetCount(&count);
        for (int i = 0; i < count; i++) {
            winrt::com_ptr<IAudioSessionControl> ctl;
            if (FAILED(list->GetSession(i, ctl.put()))) continue;

            winrt::com_ptr<IAudioSessionControl2> ctl2;
            if (FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), ctl2.put_void()))) continue;
            if (ctl2->IsSystemSoundsSession() == S_OK) continue;

            DWORD pid = 0;
            if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0) continue;
            if (!audioMatchesSlot(pid, s)) continue;

            winrt::com_ptr<ISimpleAudioVolume> vol;
            if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), vol.put_void())))
                out.push_back(vol);
        }
    }
    catch (...) {
    }
    return out;
}

struct VolumeInfo { bool available = false; int level = 0; bool mute = false; };

static VolumeInfo readVolume(const Slot& s) {
    VolumeInfo vi;
    AudioVolumes v = findAppVolumes(s);
    float maxLevel = 0;
    bool allMuted = true, any = false;
    for (auto& x : v) {
        float l = 0;
        BOOL m = FALSE;
        if (FAILED(x->GetMasterVolume(&l))) continue;
        x->GetMute(&m);
        any = true;
        maxLevel = std::max(maxLevel, l);
        if (!m) allMuted = false;
    }
    if (!any) return vi;
    vi.available = true;
    vi.level = (int)std::lround(maxLevel * 100.0f);
    vi.mute = allMuted;
    return vi;
}

// изменить громкость на deltaPercent процентов (положительное значение заодно включает звук)
static bool changeVolume(const Slot& s, int deltaPercent) {
    AudioVolumes v = findAppVolumes(s);
    bool ok = false;
    for (auto& x : v) {
        float l = 0;
        if (FAILED(x->GetMasterVolume(&l))) continue;
        int pct = std::clamp((int)std::lround(l * 100.0f) + deltaPercent, 0, 100);
        x->SetMasterVolume(pct / 100.0f, nullptr);
        if (deltaPercent > 0) x->SetMute(FALSE, nullptr);
        ok = true;
    }
    return ok;
}

// установить громкость в процентах (ненулевое значение заодно включает звук)
static bool setVolume(const Slot& s, int percent) {
    AudioVolumes v = findAppVolumes(s);
    const float f = std::clamp(percent, 0, 100) / 100.0f;
    bool ok = false;
    for (auto& x : v) {
        if (FAILED(x->SetMasterVolume(f, nullptr))) continue;
        if (percent > 0) x->SetMute(FALSE, nullptr);
        ok = true;
    }
    return ok;
}

// mode: 0 включить звук, 1 выключить, 2 переключить
static bool changeMute(const Slot& s, int mode) {
    AudioVolumes v = findAppVolumes(s);
    if (v.empty()) return false;
    BOOL cur = FALSE;
    v[0]->GetMute(&cur);
    BOOL target = (mode == 2) ? !cur : (mode == 1 ? TRUE : FALSE);
    bool ok = false;
    for (auto& x : v)
        if (SUCCEEDED(x->SetMute(target, nullptr))) ok = true;
    return ok;
}

// ----------------------------------------------------------------------------
//  Отправка на МК только изменившегося для выбранной сессии
// ----------------------------------------------------------------------------
template <typename T, typename F>
static void syncField(std::optional<T>& sent, const T& value, F&& sendFn) {
    if (sent.has_value() && *sent == value) return;
    if (sendFn()) sent = value;
}

// 1) текст, время, состояние: легко и быстро, уходит первым
static void pushText() {
    if (!g_port.isOpen() || g_selected < 0) return;
    const uint8_t id = (uint8_t)g_selected;

    syncField(g_sent.title, g_cur.title, [&] { return sendStr(CMD_TRACK_NAME, id, cutUtf8(g_cur.title, 127)); });
    syncField(g_sent.artist, g_cur.artist, [&] { return sendStr(CMD_TRACK_ARTIST, id, cutUtf8(g_cur.artist, 127)); });
    syncField(g_sent.album, g_cur.album, [&] { return sendStr(CMD_TRACK_ALBUM, id, cutUtf8(g_cur.album, 127)); });
    syncField(g_sent.dur, g_cur.dur, [&] { return sendU32(CMD_TRACK_DURATION, id, g_cur.dur); });
    syncField(g_sent.time, g_cur.time, [&] { return sendU32(CMD_TRACK_CURRENT_TIME, id, g_cur.time); });
    syncField(g_sent.state, g_cur.state, [&] { return sendU8(CMD_TRACK_STATE, id, g_cur.state); });
}

// 2) цвет фона и обложка: тяжёлая часть, уходит после текста (цвет раньше картинки)
static void pushCover() {
    if (!g_port.isOpen() || g_selected < 0) return;
    if (g_cur.cs == CoverState::Pending || g_cur.coverGen == g_sent.coverGen) return;
    const uint8_t id = (uint8_t)g_selected;

    bool ok;
    if (g_cur.cs == CoverState::Ready) {
        ok = sendColor(CMD_TRACK_BACKGROUND_COLOR, id, g_cur.color)
            && sendImage(CMD_TRACK_PHOTO, id, g_cur.cover, COVER_SIZE, COVER_SIZE);
    }
    else {
        // обложки нет: цвет по умолчанию + CMD_TRACK_PHOTO без пикселей (только id)
        // = прошивка показывает надпись «Нет обложки»
        ok = sendColor(CMD_TRACK_BACKGROUND_COLOR, id, 0)
            && sendPacket(CMD_TRACK_PHOTO, &id, 1);
    }
    if (ok) g_sent.coverGen = g_cur.coverGen;
}

// ----------------------------------------------------------------------------
//  Громкость выбранного приложения -> МК
//  CMD_VOLUME_STATE: [id][громкость 0..100][mute 0/1][доступна 0/1]
//  Приложение не играло и аудиосессии у него нет: «доступна» = 0
// ----------------------------------------------------------------------------
static void pushVolume() {
    if (!g_port.isOpen() || g_selected < 0) return;
    const uint8_t id = (uint8_t)g_selected;
    VolumeInfo vi = readVolume(g_slots[id]);
    uint32_t packed = (vi.available ? (1u << 16) : 0u) | ((uint32_t)vi.level << 8) | (vi.mute ? 1u : 0u);

    syncField(g_sent.vol, packed, [&] {
        uint8_t b[4] = { id, (uint8_t)vi.level, (uint8_t)(vi.mute ? 1 : 0), (uint8_t)(vi.available ? 1 : 0) };
        if (!sendPacket(CMD_VOLUME_STATE, b, sizeof(b))) return false;
        if (vi.available)
            logLine("[v] %s: громкость %d%%%s\n", g_slots[id].name.c_str(), vi.level, vi.mute ? " (звук выключен)" : "");
        else
            logLine("[v] %s: громкость недоступна (нет аудиосессии)\n", g_slots[id].name.c_str());
        return true;
        });
}

// ----------------------------------------------------------------------------
//  Интерфейс окна: порт, медиасессии с громкостью, обрезка Spotify, состояние дисплея
// ----------------------------------------------------------------------------
constexpr int IDC_PORT_COMBO = 2001;
constexpr int IDC_STATUS = 2002;
constexpr int IDC_CROP_CHECK = 2003;
constexpr int IDC_SESSION_LIST = 2004;
constexpr int IDC_VOLUME = 2005;
constexpr int IDC_VOLUME_LABEL = 2006;
constexpr int IDC_MUTE = 2007;
constexpr int IDC_PORT_APPLY = 2008;

static HWND g_lblPort = nullptr, g_lblSessions = nullptr, g_lblLog = nullptr;
static HWND g_portCombo = nullptr, g_cropCheck = nullptr, g_sessList = nullptr;
static HWND g_volTrack = nullptr, g_volLbl = nullptr, g_muteBtn = nullptr, g_portApply = nullptr;
static HFONT g_uiFont = nullptr;
static HFONT g_iconFont = nullptr;                // Segoe MDL2 Assets: значки динамика, как в микшере Windows
static const wchar_t* kGlyphVolume = L"\uE767";   // динамик
static const wchar_t* kGlyphMute = L"\uE74F";   // динамик перечёркнут

static int CALLBACK fontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) {
    *(bool*)p = true;
    return 0;
}

static bool fontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)fontFoundProc, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static int          g_uiSelSlot = -1;            // слот, выбранный в списке окна (не путать с g_selected на дисплее)
static std::wstring g_uiSig;                     // что сейчас показано в списке: чтобы не пересоздавать зря
static bool         g_volDragging = false;       // пользователь тянет ползунок

static std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ---- настройки (settings.ini рядом с exe) ----
static std::wstring settingsPath() { return exeDir() + L"\\settings.ini"; }

static void loadSettings() {
    std::wstring path = settingsPath();
    wchar_t buf[64];
    GetPrivateProfileStringW(L"server", L"port", L"COM8", buf, (DWORD)std::size(buf), path.c_str());
    if (buf[0]) g_portName = buf;
    g_spotifyCrop = GetPrivateProfileIntW(L"server", L"spotifyCrop", 1, path.c_str()) != 0;
}

static void saveSettings() {
    std::wstring path = settingsPath();
    WritePrivateProfileStringW(L"server", L"port", g_portName.c_str(), path.c_str());
    WritePrivateProfileStringW(L"server", L"spotifyCrop", g_spotifyCrop ? L"1" : L"0", path.c_str());
}

// ---- порты ----
static std::vector<std::wstring> listComPorts() {
    std::vector<std::wstring> v;
    wchar_t target[512];
    for (int i = 1; i <= 256; i++) {
        std::wstring n = L"COM" + std::to_wstring(i);
        if (QueryDosDeviceW(n.c_str(), target, (DWORD)std::size(target))) v.push_back(n);
    }
    return v;
}

static void refreshPortList() {
    // выбор в списке может отличаться от активного порта, пока не нажата «Подключить»
    std::wstring keep = g_portName;
    int cur = (int)SendMessageW(g_portCombo, CB_GETCURSEL, 0, 0);
    if (cur >= 0) {
        wchar_t buf[64] = {};
        SendMessageW(g_portCombo, CB_GETLBTEXT, cur, (LPARAM)buf);
        if (buf[0]) keep = buf;
    }

    auto ports = listComPorts();
    for (const std::wstring& want : { g_portName, keep })          // активный и выбранный порты остаются в списке, даже если их сейчас нет
        if (!want.empty() && std::find(ports.begin(), ports.end(), want) == ports.end()) ports.push_back(want);
    std::sort(ports.begin(), ports.end(), [](const std::wstring& a, const std::wstring& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b; // COM3 < COM10
        });

    SendMessageW(g_portCombo, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < ports.size(); i++) {
        SendMessageW(g_portCombo, CB_ADDSTRING, 0, (LPARAM)ports[i].c_str());
        if (ports[i] == keep) sel = (int)i;
    }
    if (sel >= 0) SendMessageW(g_portCombo, CB_SETCURSEL, sel, 0);
}

// кнопка «Подключить»: применяет порт, выбранный в списке
static void onPortApply() {
    int i = (int)SendMessageW(g_portCombo, CB_GETCURSEL, 0, 0);
    if (i < 0) return;
    wchar_t buf[64] = {};
    SendMessageW(g_portCombo, CB_GETLBTEXT, i, (LPARAM)buf);
    if (!buf[0]) return;

    g_portName = buf;
    saveSettings();
    cancelOpenJobs();                                               // незавершённые попытки к прежнему порту не нужны
    g_port.close();
    g_wasOpen = false;
    g_mcuSeen = false;
    g_retryDelayMs = 1000;
    g_portChanged = true;                                           // главный цикл подключится сразу, в фоновом потоке
    logLine("[*] подключение к порту %s\n", toUtf8(g_portName).c_str());
}

// ---- обрезка Spotify ----
static void onCropToggled() {
    g_spotifyCrop = SendMessageW(g_cropCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    saveSettings();
    logLine("[*] обрезка обложки Spotify: %s\n", g_spotifyCrop ? "включена" : "выключена");
    // если сейчас на дисплее Spotify, обложка пересчитывается с новыми настройками
    if (g_selected >= 0 && g_slots[g_selected].used && g_slots[g_selected].iconKey == L"spotify") {
        g_cur.cs = CoverState::Pending;
        g_cur.tries = 0;
        g_cur.nextTry = 0;
    }
}

// ---- громкость выбранного в списке приложения ----
static void setTextIfChanged(HWND h, const std::wstring& t) {
    wchar_t cur[128] = {};
    GetWindowTextW(h, cur, (int)std::size(cur));
    if (t != cur) SetWindowTextW(h, t.c_str());
}

// вертикальный ползунок: 0 сверху, поэтому значение инвертируется (100 - громкость)
static void updateVolumeControl() {
    if (g_volDragging) return;
    if (g_uiSelSlot < 0 || !g_slots[g_uiSelSlot].used) {
        EnableWindow(g_volTrack, FALSE);
        EnableWindow(g_muteBtn, FALSE);
        setTextIfChanged(g_volLbl, L"");
        return;
    }
    VolumeInfo vi = readVolume(g_slots[g_uiSelSlot]);
    if (!vi.available) {                                            // приложение ещё не воспроизводило звук
        EnableWindow(g_volTrack, FALSE);
        EnableWindow(g_muteBtn, FALSE);
        setTextIfChanged(g_volLbl, L"нет звука");
        return;
    }
    EnableWindow(g_volTrack, TRUE);
    EnableWindow(g_muteBtn, TRUE);
    SendMessageW(g_volTrack, TBM_SETPOS, TRUE, 100 - vi.level);
    setTextIfChanged(g_volLbl, vi.mute ? L"без звука" : std::to_wstring(vi.level) + L"%");
    setTextIfChanged(g_muteBtn, g_iconFont ? (vi.mute ? kGlyphMute : kGlyphVolume)
        : (vi.mute ? L"Вкл. звук" : L"Выкл. звук"));
}

// кнопка mute под ползунком
static void onMuteClicked() {
    if (g_uiSelSlot < 0 || !g_slots[g_uiSelSlot].used) return;
    changeMute(g_slots[g_uiSelSlot], 2);                            // переключить
    if (g_uiSelSlot == g_selected) pushVolume();                    // дисплей сразу узнаёт состояние
    updateVolumeControl();
}

static void applyUiVolume(bool force) {
    if (g_uiSelSlot < 0 || !g_slots[g_uiSelSlot].used) return;
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!force && now - last < 40) return;                         // не чаще 25 раз в секунду при перетаскивании
    last = now;

    int vol = 100 - (int)SendMessageW(g_volTrack, TBM_GETPOS, 0, 0);
    setVolume(g_slots[g_uiSelSlot], vol);
    setTextIfChanged(g_volLbl, std::to_wstring(vol) + L"%");
    if (g_uiSelSlot == g_selected) pushVolume();                   // дисплей сразу узнаёт новое значение
}

static void onUiVScroll(HWND from, int code) {
    if (from != g_volTrack) return;
    if (code == TB_THUMBTRACK) g_volDragging = true;
    else if (code == TB_ENDTRACK) g_volDragging = false;
    applyUiVolume(code == TB_ENDTRACK);
}

static void onSessionPicked() {
    int i = (int)SendMessageW(g_sessList, LB_GETCURSEL, 0, 0);
    g_uiSelSlot = i >= 0 ? (int)SendMessageW(g_sessList, LB_GETITEMDATA, i, 0) : -1;
    updateVolumeControl();
}

static void handleSelect(int id);                                  // определена ниже, в разделе «Слоты»

// двойной щелчок по приложению: показать его сессию на дисплее
static void onSessionDoubleClick() {
    int i = (int)SendMessageW(g_sessList, LB_GETCURSEL, 0, 0);
    if (i < 0) return;
    int id = (int)SendMessageW(g_sessList, LB_GETITEMDATA, i, 0);
    if (id < 0 || id >= MAX_SESSIONS || !g_slots[id].used) return;
    if (!g_port.isOpen() || !g_slots[id].known) {
        logLine("[!] дисплей не подключён: показать сессию нельзя\n");
        return;
    }
    uint8_t p[1] = { (uint8_t)id };
    sendPacket(CMD_SELECT_MEDIASESSION, p, 1);                      // дисплей переключит выбранную сессию
    handleSelect(id);                                               // и сразу получит её данные: текст, обложку, громкость
}

static void onUiCommand(int id, int code) {
    switch (id) {
    case IDC_PORT_COMBO:
        if (code == CBN_DROPDOWN) refreshPortList();           // свежий список портов при каждом раскрытии
        break;                                                 // сам выбор порта применяется кнопкой «Подключить»
    case IDC_PORT_APPLY:
        if (code == BN_CLICKED) onPortApply();
        break;
    case IDC_CROP_CHECK:
        if (code == BN_CLICKED) onCropToggled();
        break;
    case IDC_SESSION_LIST:
        if (code == LBN_SELCHANGE) onSessionPicked();
        else if (code == LBN_DBLCLK) onSessionDoubleClick();
        break;
    case IDC_MUTE:
        if (code == BN_CLICKED) onMuteClicked();
        break;
    }
}

// ---- обновление окна: вызывается из главного цикла дважды в секунду ----
static void uiUpdate() {
    if (!g_hwnd || !g_edit) return;

    // состояние дисплея: порт закрыт / порт открыт, но МК молчит / подключен
    int st = g_port.isOpen() ? (g_mcuSeen ? 2 : 1) : (hasPendingJob(g_portName) ? 3 : 0);
    if (st != g_uiStatus) {
        g_uiStatus = st;
        SetWindowTextW(g_statusLbl, st == 2 ? L"Дисплей: подключен"
            : st == 1 ? L"Дисплей: порт открыт, дисплей не отвечает"
            : st == 3 ? L"Дисплей: подключение к порту…"
            : L"Дисплей: порт не открыт");
    }

    // список активных медиасессий
    std::vector<std::pair<int, std::wstring>> items;
    std::wstring sig;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_slots[i].used) continue;
        std::wstring label = fromUtf8(g_slots[i].name);
        if (i == g_selected) label += L"   (на дисплее)";
        items.emplace_back(i, label);
        sig += std::to_wstring(i) + L":" + label + L"|";
    }
    if (sig != g_uiSig) {
        g_uiSig = sig;
        SendMessageW(g_sessList, WM_SETREDRAW, FALSE, 0);
        SendMessageW(g_sessList, LB_RESETCONTENT, 0, 0);
        int want = -1;
        for (size_t k = 0; k < items.size(); k++) {
            int idx = (int)SendMessageW(g_sessList, LB_ADDSTRING, 0, (LPARAM)items[k].second.c_str());
            SendMessageW(g_sessList, LB_SETITEMDATA, idx, (LPARAM)items[k].first);
            if (items[k].first == g_uiSelSlot) want = (int)k;
        }
        if (want < 0 && !items.empty()) {                          // прежний выбор исчез: берём ту, что на дисплее, иначе первую
            want = 0;
            for (size_t k = 0; k < items.size(); k++)
                if (items[k].first == g_selected) want = (int)k;
        }
        if (want >= 0) SendMessageW(g_sessList, LB_SETCURSEL, want, 0);
        g_uiSelSlot = want >= 0 ? items[want].first : -1;
        SendMessageW(g_sessList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_sessList, nullptr, TRUE);
    }
    updateVolumeControl();
}

// ---- создание и раскладка элементов ----
static HWND mkChild(HINSTANCE hi, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
        g_hwnd, (HMENU)(INT_PTR)id, hi, nullptr);
    if (g_uiFont) SendMessageW(h, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    return h;
}

static void createControls(HINSTANCE hi) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);        // шрифт, как в остальных окнах Windows

    wchar_t cropText[96];
    swprintf_s(cropText, L"Обрезать обложку Spotify (%d%% снизу)", (int)std::lround(SPOTIFY_CROP_BOTTOM * 100));

    g_lblPort = mkChild(hi, L"STATIC", L"Порт:", SS_LEFT, 0);
    g_portCombo = mkChild(hi, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_PORT_COMBO);
    g_portApply = mkChild(hi, L"BUTTON", L"Подключить", BS_PUSHBUTTON | WS_TABSTOP, IDC_PORT_APPLY);
    g_statusLbl = mkChild(hi, L"STATIC", L"", SS_LEFT, IDC_STATUS);
    g_cropCheck = mkChild(hi, L"BUTTON", cropText, BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CROP_CHECK);
    g_lblSessions = mkChild(hi, L"STATIC", L"Активные медиасессии и громкость выбранного приложения:", SS_LEFT, 0);
    g_sessList = mkChild(hi, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
        IDC_SESSION_LIST, WS_EX_CLIENTEDGE);
    g_volTrack = mkChild(hi, TRACKBAR_CLASSW, L"", TBS_VERT | TBS_NOTICKS | WS_TABSTOP, IDC_VOLUME);
    g_volLbl = mkChild(hi, L"STATIC", L"", SS_CENTER, IDC_VOLUME_LABEL);

    // кнопка mute: значок динамика (шрифт Segoe MDL2 Assets), а если шрифта нет, текст
    if (fontExists(L"Segoe MDL2 Assets"))
        g_iconFont = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    g_muteBtn = mkChild(hi, L"BUTTON", g_iconFont ? kGlyphVolume : L"Выкл. звук", BS_PUSHBUTTON | WS_TABSTOP, IDC_MUTE);
    if (g_iconFont) SendMessageW(g_muteBtn, WM_SETFONT, (WPARAM)g_iconFont, TRUE);
    g_lblLog = mkChild(hi, L"STATIC", L"Журнал:", SS_LEFT, 0);

    SendMessageW(g_cropCheck, BM_SETCHECK, g_spotifyCrop ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_volTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(g_volTrack, TBM_SETLINESIZE, 0, 1);
    SendMessageW(g_volTrack, TBM_SETPAGESIZE, 0, 10);
    EnableWindow(g_volTrack, FALSE);
    EnableWindow(g_muteBtn, FALSE);

    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 10, 10, g_hwnd, nullptr, hi, nullptr);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 200000, 0);
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, TRUE);

    refreshPortList();
}

static void layoutControls(int cw, int ch) {
    if (!g_edit) return;                                           // элементы ещё не созданы
    const int M = 10;
    int y = M;

    MoveWindow(g_lblPort, M, y + 4, 40, 20, TRUE);
    MoveWindow(g_portCombo, M + 44, y, 120, 240, TRUE);            // высота включает выпадающий список
    MoveWindow(g_portApply, M + 44 + 120 + 8, y - 1, 100, 26, TRUE);
    y += 34;

    MoveWindow(g_statusLbl, M, y, std::max(50, cw - 2 * M), 20, TRUE);
    y += 30;

    MoveWindow(g_cropCheck, M, y, std::max(50, cw - 2 * M), 22, TRUE);
    y += 34;

    MoveWindow(g_lblSessions, M, y, std::max(50, cw - 2 * M), 20, TRUE);
    y += 22;

    const int listH = 170, volW = 70;
    const int listW = std::max(100, cw - 2 * M - volW - 8);
    MoveWindow(g_sessList, M, y, listW, listH, TRUE);
    const int vx = M + listW + 8;
    MoveWindow(g_volTrack, vx + (volW - 40) / 2, y, 40, listH - 58, TRUE);   // ползунок справа от списка
    MoveWindow(g_volLbl, vx, y + listH - 56, volW, 20, TRUE);
    MoveWindow(g_muteBtn, vx + (volW - 44) / 2, y + listH - 32, 44, 32, TRUE);   // mute под ползунком
    y += listH + 14;

    MoveWindow(g_lblLog, M, y, 100, 20, TRUE);
    y += 22;
    MoveWindow(g_edit, M, y, std::max(50, cw - 2 * M), std::max(40, ch - y - M), TRUE);
}

// ----------------------------------------------------------------------------
//  Опрос выбранной сессии
// ----------------------------------------------------------------------------
static void updateSelected() {
    if (g_selected < 0) return;
    Slot& sl = g_slots[g_selected];
    if (!sl.used || !sl.session) return;

    try {
        // ---- свойства трека ----
        auto props = sl.session.TryGetMediaPropertiesAsync().get();
        g_cur.title = u8(props.Title());
        g_cur.artist = u8(props.Artist());
        g_cur.album = u8(props.AlbumTitle());

        std::wstring key(props.Title().c_str());
        key += L'\x1f'; key += props.Artist().c_str();
        key += L'\x1f'; key += props.AlbumTitle().c_str();
        if (key != g_cur.key) {                       // трек сменился
            g_cur.key = key;
            g_cur.cs = CoverState::Pending;
            g_cur.tries = 0;
            g_cur.nextTry = 0;
        }

        // ---- состояние воспроизведения ----
        auto info = sl.session.GetPlaybackInfo();
        switch (info.PlaybackStatus()) {
        case PStatus::Playing:  g_cur.state = 1; break;
        case PStatus::Paused:   g_cur.state = 2; break;
        case PStatus::Changing: break;            // оставляем прежнее
        default:                g_cur.state = 0; break;
        }

        // ---- таймлайн ----
        using namespace std::chrono;
        auto tl = sl.session.GetTimelineProperties();
        int64_t start = duration_cast<seconds>(tl.StartTime()).count();
        int64_t end = duration_cast<seconds>(tl.EndTime()).count();
        int64_t pos = duration_cast<seconds>(tl.Position()).count() - start;
        int64_t dur = end - start;
        if (dur < 0) dur = 0;
        if (g_cur.state == 1) {                       // позиция устаревает: досчитываем
            int64_t el = duration_cast<seconds>(winrt::clock::now() - tl.LastUpdatedTime()).count();
            if (el >= 0 && el < 6 * 3600) pos += el;
        }
        if (pos < 0) pos = 0;
        if (dur > 0 && pos > dur) pos = dur;
        g_cur.dur = (uint32_t)dur;
        g_cur.time = (uint32_t)pos;

        // 1) сначала на МК уходят название, исполнитель, время и состояние
        pushText();

        // 2) затем обложка (декодирование картинки занимает время, текст его не ждёт).
        //    Миниатюра может появиться с задержкой: пробуем ~3 с.
        if (g_cur.cs == CoverState::Pending && GetTickCount64() >= g_cur.nextTry) {
            bool ok = false;
            auto thumb = props.Thumbnail();
            if (thumb) {
                std::vector<uint8_t> px;
                uint16_t avg = 0;
                if (makeCover(thumb, px, avg, cropFor(sl.iconKey))) {
                    g_cur.cover = std::move(px);
                    g_cur.color = avg;
                    ok = true;
                    if (g_dumpThumbs) dumpThumbnail(thumb, sl.iconKey);
                }
            }
            if (ok) {
                g_cur.cs = CoverState::Ready;
                g_cur.coverGen++;
            }
            else if (++g_cur.tries >= 6) {
                g_cur.cs = CoverState::None;
                g_cur.cover.clear();
                g_cur.color = 0;
                g_cur.coverGen++;
            }
            else {
                g_cur.nextTry = GetTickCount64() + 500;
            }
        }
        pushCover();                                  // цвет фона, затем картинка
    }
    catch (hresult_error const&) {
        // сессия закрылась или приложение не отвечает: пропускаем цикл
    }
}

// ----------------------------------------------------------------------------
//  Слоты: создание, удаление, синхронизация с Windows
// ----------------------------------------------------------------------------
static void sendCreate(int i) {
    Slot& s = g_slots[i];
    if (!g_port.isOpen()) return;
    bool ok = sendStr(CMD_CREATE_MEDIASESSION, (uint8_t)i, cutUtf8(s.name, 47))
        && sendColor(CMD_APPS_BACKGROUND_COLOR, (uint8_t)i, s.color);
    if (ok && !s.icon.empty())
        sendImage(CMD_APP_ICON, (uint8_t)i, s.icon, ICON_SIZE, ICON_SIZE);   // после CREATE
    s.known = ok;
}

static void deselect() {
    g_selected = -1;
    g_cur = Track{};
    g_sent = Sent{};
}

// Менеджер медиасессий Windows. Подписка на SessionsChanged и периодический повторный запрос
// защищают от «застывшего» списка: объект менеджера может перестать обновляться.
static Manager g_mgr{ nullptr };
static winrt::event_token g_mgrToken{};
static std::atomic<bool> g_sessionsDirty{ false };
static ULONGLONG g_mgrRequestedAt = 0;

static bool requestManager() {
    g_mgrRequestedAt = GetTickCount64();
    try {
        Manager m = Manager::RequestAsync().get();
        if (g_mgr) {
            try { g_mgr.SessionsChanged(g_mgrToken); }
            catch (...) {}      // отписаться от прежнего объекта
        }
        g_mgr = m;
        g_mgrToken = g_mgr.SessionsChanged(
            Windows::Foundation::TypedEventHandler<Manager, SessionsChangedEventArgs>(
                [](Manager const&, SessionsChangedEventArgs const&) { g_sessionsDirty = true; }));
        return true;
    }
    catch (hresult_error const& e) {
        logLine("[!] не удалось получить менеджер медиасессий: %s\n", toUtf8(std::wstring(e.message().c_str())).c_str());
        return false;
    }
}

static void syncSessions() {
    if (!g_mgr) return;
    std::array<bool, MAX_SESSIONS> seen{};
    try {
        auto winSessions = g_mgr.GetSessions();
        static int lastCount = -1;
        if ((int)winSessions.Size() != lastCount) {                       // диагностика: что именно сообщает Windows
            lastCount = (int)winSessions.Size();
            logLine("[~] Windows сообщает о медиасессиях: %d\n", lastCount);
        }
        for (auto const& s : winSessions) {
            std::wstring appId(s.SourceAppUserModelId().c_str());

            int idx = -1;
            for (int i = 0; i < MAX_SESSIONS; i++)
                if (g_slots[i].used && g_slots[i].appId == appId) { idx = i; break; }

            bool isNew = false;
            if (idx < 0) {
                for (int i = 0; i < MAX_SESSIONS; i++)
                    if (!g_slots[i].used) { idx = i; break; }
                if (idx < 0) continue;                // свободных слотов нет
                Slot& n = g_slots[idx];
                n.used = true;
                n.known = false;
                n.appId = appId;
                AppInfo ai = lookupApp(appId);
                n.name = ai.name;
                n.color = colorFromString(appId);
                n.iconKey = ai.iconKey;
                n.icon = loadAppIcon(ai.iconKey, n.color);
                isNew = true;
                logLine("[+] слот %d: %s [%s]\n", idx, n.name.c_str(),
                    n.icon.empty() ? ("нет иконки: icons\\" + toUtf8(ai.iconKey) + ".png").c_str()
                    : "иконка загружена");
            }
            g_slots[idx].session = s;                 // актуальный объект сессии
            seen[idx] = true;
            if (isNew) sendCreate(idx);
        }
    }
    catch (hresult_error const& e) {
        static ULONGLONG lastLog = 0;                 // не засорять журнал повторами
        ULONGLONG t = GetTickCount64();
        if (t - lastLog > 10000) {
            lastLog = t;
            logLine("[!] ошибка чтения медиасессий: %s\n", toUtf8(std::wstring(e.message().c_str())).c_str());
        }
        g_mgrRequestedAt = 0;                         // следующий проход заново запросит менеджер
        return;                                       // при ошибке слоты не трогаем
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        Slot& s = g_slots[i];
        if (!s.used || seen[i]) continue;
        logLine("[-] слот %d: %s\n", i, s.name.c_str());
        if (g_port.isOpen()) { uint8_t p[1] = { (uint8_t)i }; sendPacket(CMD_DELETE_MEDIASESSION, p, 1); }
        if (g_selected == i) deselect();              // МК сам выберет другую и пришлёт SELECT
        s = Slot{};
    }
}

// полная пересинхронизация МК: чистим все слоты и создаём заново
static void doResync() {
    if (!g_port.isOpen()) return;
    g_lastResync = GetTickCount64();
    deselect();
    for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
        uint8_t p[1] = { i };
        sendPacket(CMD_DELETE_MEDIASESSION, p, 1);
        g_slots[i].known = false;
    }
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_slots[i].used) sendCreate(i);           // МК сам выберет первый и пришлёт SELECT
    if (g_port.isOpen()) {
        g_needResync = false;
        logLine("[*] МК синхронизирован\n");
    }
}

// управление треком по запросу МК (CMD_CONTROL)
static bool runControl(int id, int action) {
    if (id < 0 || id >= MAX_SESSIONS || !g_slots[id].used || !g_slots[id].session) return false;

    const char* what = "неизвестное действие";
    bool ok = false;
    try {
        Session s = g_slots[id].session;
        Windows::Foundation::IAsyncOperation<bool> op{ nullptr };
        switch (action) {
        case CTRL_PLAY_PAUSE: what = "играть/пауза";       op = s.TryTogglePlayPauseAsync(); break;
        case CTRL_NEXT:       what = "следующий трек";     op = s.TrySkipNextAsync();        break;
        case CTRL_PREV:       what = "предыдущий трек";    op = s.TrySkipPreviousAsync();    break;
        case CTRL_PLAY:       what = "играть";             op = s.TryPlayAsync();            break;
        case CTRL_PAUSE:      what = "пауза";              op = s.TryPauseAsync();           break;
        case CTRL_STOP:       what = "стоп";               op = s.TryStopAsync();            break;
        }
        // приложение может не отвечать: ждём не дольше 1,5 секунды, чтобы не вешать программу
        if (op && op.wait_for(std::chrono::milliseconds(1500)) == Windows::Foundation::AsyncStatus::Completed)
            ok = op.GetResults();
    }
    catch (hresult_error const&) {
    }
    logLine("[c] %s: %s%s\n", g_slots[id].name.c_str(), what, ok ? "" : " (не выполнено)");
    return ok;
}

// МК выбрал сессию: отправляем полный снимок
static void handleSelect(int id) {
    if (id < 0 || id >= MAX_SESSIONS) return;
    if (!g_slots[id].used || !g_slots[id].known) return;   // устаревший SELECT
    g_selected = id;
    g_cur = Track{};
    g_sent = Sent{};
    logLine("[>] выбран слот %d: %s\n", id, g_slots[id].name.c_str());
    updateSelected();
    pushVolume();
}

// ----------------------------------------------------------------------------
//  main
// ----------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    // аргументы: [COM-порт] [--tray] [--dump-thumbs]
    loadSettings();                                    // порт и галочка из settings.ini
    bool startHidden = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--tray")             startHidden = true;
        else if (a == L"--dump-thumbs") g_dumpThumbs = true;
        else                            g_portName = a;       // порт из командной строки важнее settings.ini
    }

    // одна копия программы: повторный запуск просто показывает окно уже работающей
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\ExternalDisplayMediaServer");
    (void)instanceMutex;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kWndClass, nullptr);
        if (other) PostMessageW(other, WM_TRAY, TRAY_ID, WM_LBUTTONUP);
        return 0;
    }

    init_apartment();                                  // MTA: разрешает .get() без цикла сообщений WinRT
    crcInit();
    if (!createMainWindow(GetModuleHandleW(nullptr), startHidden)) return 1;

    logLine("Порт: %s. Крестик окна сворачивает программу в трей, выход через значок в трее.\n",
        toUtf8(g_portName).c_str());
    if (g_dumpThumbs) logLine("[d] режим --dump-thumbs: исходные миниатюры сохраняются в thumbs\\\n");

    ULONGLONG lastTry = 0, lastSync = 0, lastUpd = 0;

    while (g_running) {
        pumpMessages();
        ULONGLONG now = GetTickCount64();

        // ---- подключение (порт открывается в фоновом потоке, окно не зависает) ----
        if (collectOpenJobs()) {
            logLine("[*] порт %s открыт\n", toUtf8(g_portName).c_str());
            g_mcuSeen = false;
            g_parser.reset();
            deselect();
            g_needResync = true;
        }
        bool openNow = g_port.isOpen();
        if (g_wasOpen && !openNow) {                   // порт закрылся: если МК так и не ответил, пауза растёт (до 15 с)
            g_retryDelayMs = g_mcuSeen ? 1000 : std::min<ULONGLONG>(g_retryDelayMs * 2, 15000);
            lastTry = now;
        }
        g_wasOpen = openNow;

        if (!openNow) {
            if ((now - lastTry >= g_retryDelayMs || g_portChanged) && !hasPendingJob(g_portName)) {
                lastTry = now;
                g_portChanged = false;
                startOpenJob(g_portName);
            }
        }
        else {
            uint8_t buf[256];
            size_t n;
            while ((n = g_port.read(buf, sizeof(buf))) > 0)
                for (size_t i = 0; i < n; i++) g_parser.feed(buf[i]);
            if (!g_port.isOpen()) logLine("[!] порт закрыт, ждём переподключения\n");
        }

        // ---- входящие от МК ----
        int pendingSelect = -1;
        int volId = -1, volDelta = 0, muteId = -1, muteMode = -1;
        std::vector<std::pair<int, int>> ctrlReqs;                 // (слот, действие) из CMD_CONTROL
        while (!g_parser.out.empty()) {
            Incoming p = std::move(g_parser.out.front());
            g_parser.out.pop_front();
            g_mcuSeen = true;                                      // дисплей отвечает
            if (p.cmd == CMD_CONFIGURATION) {
                if (now - g_lastResync > 1500) g_needResync = true;   // МК просит всё заново
            }
            else if (p.cmd == CMD_SELECT_MEDIASESSION && !p.payload.empty()) {
                pendingSelect = p.payload[0];                          // нужен только последний
            }
            else if (p.cmd == CMD_VOLUME_STEP && p.payload.size() >= 2) {
                if (volId != p.payload[0]) { volId = p.payload[0]; volDelta = 0; }
                volDelta += (int8_t)p.payload[1];                      // шаги энкодера суммируем
            }
            else if (p.cmd == CMD_VOLUME_MUTE && p.payload.size() >= 2) {
                muteId = p.payload[0];
                muteMode = p.payload[1];
            }
            else if (p.cmd == CMD_CONTROL && p.payload.size() >= 2) {
                ctrlReqs.emplace_back(p.payload[0], p.payload[1]);     // каждое нажатие выполняется отдельно
            }
        }

        // ---- слоты ----
        if (now - g_mgrRequestedAt > 5000) requestManager();       // менеджер периодически запрашивается заново
        if (now - lastSync >= 500 || g_needResync || g_sessionsDirty) {
            lastSync = now;
            g_sessionsDirty = false;
            syncSessions();
        }
        if (g_needResync && g_port.isOpen()) doResync();

        if (pendingSelect >= 0 && !g_needResync) handleSelect(pendingSelect);

        // ---- громкость по запросу МК ----
        if (!g_needResync) {
            volDelta = std::clamp(volDelta, -100, 100);
            if (volDelta != 0 && volId >= 0 && volId < MAX_SESSIONS && g_slots[volId].used && g_slots[volId].known) {
                changeVolume(g_slots[volId], volDelta);
                if (volId == g_selected) pushVolume();
            }
            if (muteMode >= 0 && muteId >= 0 && muteId < MAX_SESSIONS && g_slots[muteId].used && g_slots[muteId].known) {
                changeMute(g_slots[muteId], muteMode);
                if (muteId == g_selected) pushVolume();
            }
        }

        // ---- управление треком по запросу МК ----
        if (!g_needResync && !ctrlReqs.empty()) {
            for (auto& rq : ctrlReqs) runControl(rq.first, rq.second);
            lastUpd = now - 350;                                   // состояние скоро перечитается: приложению нужно мгновение
        }

        // ---- выбранная сессия и окно ----
        if (now - lastUpd >= 500) {
            lastUpd = now;
            updateSelected();
            pushVolume();
            uiUpdate();
        }

        Sleep(15);
    }

    g_port.close();
    trayRemove();
    DestroyWindow(g_hwnd);
    return 0;
}