// ============================================================================
//  media_server.cpp  —  серверная часть для дисплея «Внешний дисплей»
//
//  Читает медиасессии Windows (GlobalSystemMediaTransportControls), ведёт
//  таблицу «приложение -> слот 0..15» и общается с RP2040 по USB CDC.
//
//  Сборка (x64 Native Tools Command Prompt for VS, Windows 10 SDK >= 17763):
//      cl /std:c++20 /EHsc /utf-8 /permissive- media_server.cpp
//  Запуск:
//      media_server.exe [COM8]
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "windowsapp")

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

    bool open(const std::wstring& name) {
        close();
        std::wstring path = L"\\\\.\\" + name;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
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
        if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;   // ReadFile возвращает сразу то, что есть
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant = 0;
        to.WriteTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant = 3000;
        SetCommTimeouts(h, &to);
        SetupComm(h, 1 << 16, 1 << 16);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        EscapeCommFunction(h, SETDTR);

        h_ = h;
        return true;
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
    uint32_t coverGen = 0;
};
static Sent g_sent;

static bool g_needResync = false;
static ULONGLONG g_lastResync = 0;

// ----------------------------------------------------------------------------
//  Обложка: уменьшить до 128x128 (с обрезкой по центру) и перевести в RGB565
// ----------------------------------------------------------------------------
static bool makeCover(IRandomAccessStreamReference const& thumb, std::vector<uint8_t>& out, uint16_t& avgColor) {
    try {
        auto stream = thumb.OpenReadAsync().get();
        auto decoder = BitmapDecoder::CreateAsync(stream).get();

        uint32_t w = decoder.PixelWidth(), h = decoder.PixelHeight();
        if (!w || !h) return false;

        // масштабируем так, чтобы меньшая сторона = 128, затем режем центр
        double scale = double(COVER_SIZE) / std::min(w, h);
        uint32_t sw = std::max<uint32_t>(COVER_SIZE, (uint32_t)(w * scale + 0.5));
        uint32_t sh = std::max<uint32_t>(COVER_SIZE, (uint32_t)(h * scale + 0.5));

        BitmapTransform tr;
        tr.ScaledWidth(sw);
        tr.ScaledHeight(sh);
        tr.InterpolationMode(BitmapInterpolationMode::Fant);
        tr.Bounds(BitmapBounds{ (sw - COVER_SIZE) / 2, (sh - COVER_SIZE) / 2, COVER_SIZE, COVER_SIZE });  // после масштабирования

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
                if (makeCover(thumb, px, avg)) { g_cur.cover = std::move(px); g_cur.color = avg; ok = true; }
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

static void syncSessions(Manager const& mgr) {
    std::array<bool, MAX_SESSIONS> seen{};
    try {
        for (auto const& s : mgr.GetSessions()) {
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
                n.icon = loadAppIcon(ai.iconKey, n.color);
                isNew = true;
                printf("[+] session %d: %s [%s]\n", idx, n.name.c_str(),
                    n.icon.empty() ? ("no icon: icons\\" + toUtf8(ai.iconKey) + ".png").c_str()
                    : "icon loaded");
            }
            g_slots[idx].session = s;                 // актуальный объект сессии
            seen[idx] = true;
            if (isNew) sendCreate(idx);
        }
    }
    catch (hresult_error const&) {
        return;                                       // при ошибке слоты не трогаем
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        Slot& s = g_slots[i];
        if (!s.used || seen[i]) continue;
        printf("[-] session %d: %s\n", i, s.name.c_str());
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
        printf("[*] MC synchronized\n");
    }
}

// МК выбрал сессию: отправляем полный снимок
static void handleSelect(int id) {
    if (id < 0 || id >= MAX_SESSIONS) return;
    if (!g_slots[id].used || !g_slots[id].known) return;   // устаревший SELECT
    g_selected = id;
    g_cur = Track{};
    g_sent = Sent{};
    printf("[>] selected session %d: %s\n", id, g_slots[id].name.c_str());
    updateSelected();
}

// ----------------------------------------------------------------------------
//  main
// ----------------------------------------------------------------------------
static std::atomic<bool> g_running{ true };
static BOOL WINAPI onCtrl(DWORD) { g_running = false; return TRUE; }

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    init_apartment();                                  // MTA: разрешает .get() в консоли
    crcInit();

    std::wstring portName = argc > 1 ? argv[1] : L"COM8";
    printf("Port: %s. Ctrl+C for exit.\n", toUtf8(portName).c_str());

    Manager mgr = Manager::RequestAsync().get();

    ULONGLONG lastTry = 0, lastSync = 0, lastUpd = 0;

    while (g_running) {
        ULONGLONG now = GetTickCount64();

        // ---- подключение ----
        if (!g_port.isOpen()) {
            if (now - lastTry >= 1000) {
                lastTry = now;
                if (g_port.open(portName)) {
                    printf("[*] Port opened\n");
                    g_parser.reset();
                    deselect();
                    g_needResync = true;
                }
            }
        }
        else {
            uint8_t buf[256];
            size_t n;
            while ((n = g_port.read(buf, sizeof(buf))) > 0)
                for (size_t i = 0; i < n; i++) g_parser.feed(buf[i]);
            if (!g_port.isOpen()) printf("[!] Port closed, waiting for reconnection\n");
        }

        // ---- входящие от МК ----
        int pendingSelect = -1;
        while (!g_parser.out.empty()) {
            Incoming p = std::move(g_parser.out.front());
            g_parser.out.pop_front();
            if (p.cmd == CMD_CONFIGURATION) {
                if (now - g_lastResync > 1500) g_needResync = true;   // МК просит всё заново
            }
            else if (p.cmd == CMD_SELECT_MEDIASESSION && !p.payload.empty()) {
                pendingSelect = p.payload[0];                          // нужен только последний
            }
        }

        // ---- слоты ----
        if (now - lastSync >= 500 || g_needResync) {
            lastSync = now;
            syncSessions(mgr);
        }
        if (g_needResync && g_port.isOpen()) doResync();

        if (pendingSelect >= 0 && !g_needResync) handleSelect(pendingSelect);

        // ---- выбранная сессия ----
        if (now - lastUpd >= 500) {
            lastUpd = now;
            updateSelected();
        }

        Sleep(15);
    }

    g_port.close();
    return 0;
}