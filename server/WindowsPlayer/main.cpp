#define NOMINMAX
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <iostream>
#include <windows.h>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <algorithm>

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

// Глобальные переменные состояния и синхронизации
HANDLE hSerial_global = INVALID_HANDLE_VALUE;
std::mutex serial_mutex;   // защита записи в порт и last_* переменных
std::mutex session_mutex;  // защита current_session (доступ из разных потоков)

std::string last_title = "";
std::string last_artist = "";
std::string last_album = "";
std::string last_app = "";
std::string last_state = "";
std::string last_timeline = "";

// Токены подписок и текущая сессия
event_token mediaPropsToken;
event_token playbackInfoToken;
GlobalSystemMediaTransportControlsSession current_session{ nullptr };

// CRC8 calculation function
uint8_t calculate_crc8(const uint8_t* data, size_t len) {
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

// Send text packet to RP2040 (Оптимизация: резервирование памяти вектора)
void sendTextPacket(HANDLE hSerial, uint8_t cmd, const std::string& text) {
    if (hSerial == INVALID_HANDLE_VALUE) return;
    uint16_t length = (uint16_t)text.length();

    std::vector<uint8_t> packet;
    packet.reserve(5 + length + 1);
    packet.push_back(0xAA);
    packet.push_back(0xBB);
    packet.push_back(cmd);
    packet.push_back((uint8_t)((length >> 8) & 0xFF));
    packet.push_back((uint8_t)(length & 0xFF));

    if (length > 0) {
        packet.insert(packet.end(), text.begin(), text.end());
    }

    uint8_t crc = calculate_crc8((const uint8_t*)text.data(), length);
    packet.push_back(crc);

    DWORD bytesWritten = 0;
    WriteFile(hSerial, packet.data(), (DWORD)packet.size(), &bytesWritten, NULL);

    // ПРИНУДИТЕЛЬНО СБРАСЫВАЕМ БУФЕР, ЧТОБЫ ДАННЫЕ СРАЗУ ШЛИ НА МИКРОКОНТРОЛЛЕР
    FlushFileBuffers(hSerial);
}

// Форматирование секунд: m:ss или h:mm:ss
std::string formatTime(long long totalSeconds) {
    if (totalSeconds < 0) totalSeconds = 0;
    long long h = totalSeconds / 3600;
    long long m = (totalSeconds % 3600) / 60;
    long long s = totalSeconds % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    else       snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    return buf;
}

// Формирует строку вида "1:30 ===------- 5:30"
std::string buildTimelineString(GlobalSystemMediaTransportControlsSession const& session) {
    using namespace std::chrono;

    if (!session) return "";

    auto timeline = session.GetTimelineProperties();
    if (!timeline) return "";

    long long startMs = duration_cast<milliseconds>(timeline.StartTime()).count();
    long long endMs = duration_cast<milliseconds>(timeline.EndTime()).count();
    long long posMs = duration_cast<milliseconds>(timeline.Position()).count();

    long long durMs = endMs - startMs;
    posMs -= startMs;

    // Windows обновляет Position только при событиях (пауза, перемотка и т.п.),
    // поэтому при воспроизведении добавляем время, прошедшее с LastUpdatedTime
    auto playbackInfo = session.GetPlaybackInfo();
    if (playbackInfo &&
        playbackInfo.PlaybackStatus() ==
        GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
        auto elapsed = winrt::clock::now() - timeline.LastUpdatedTime();
        posMs += duration_cast<milliseconds>(elapsed).count();
    }

    if (durMs < 0) durMs = 0;
    if (posMs < 0) posMs = 0;
    if (durMs == 0) posMs = 0;                 // длительность неизвестна - позиция бессмысленна
    if (posMs > durMs) posMs = durMs;

    // 10 символов шкалы (с округлением)
    int filled = 0;
    if (durMs > 0) {
        filled = (int)((posMs * 20 + durMs / 2) / durMs);
        filled = (std::max)(0, (std::min)(20, filled));
    }

    std::string bar(filled, '=');
    bar.append(20 - filled, '-');

    return formatTime(posMs / 1000) + " " + bar + " " + formatTime(durMs / 1000);
}

// Отправка строки состояния трека (команда 0x06), только если она изменилась
void sendTimelineData() {
    try {
        GlobalSystemMediaTransportControlsSession session{ nullptr };
        {
            std::lock_guard<std::mutex> lock(session_mutex);
            session = current_session;
        }

        std::string line = buildTimelineString(session);

        std::lock_guard<std::mutex> lock(serial_mutex);
        if (line != last_timeline) {
            sendTextPacket(hSerial_global, 0x06, line);
            last_timeline = line;
        }
    }
    catch (...) {
        // Сессия могла закрыться в момент чтения
    }
}

// Обработка и отправка актуальных данных сессии
void processAndSendSessionData(GlobalSystemMediaTransportControlsSession const& session) {
    if (!session) return;

    std::string title = "";
    std::string artist = "";
    std::string album = "";
    std::string trackState = "0";

    try {
        std::string appAumid = winrt::to_string(session.SourceAppUserModelId());
        auto props = session.TryGetMediaPropertiesAsync().get();
        auto playbackInfo = session.GetPlaybackInfo();

        if (props) {
            title = winrt::to_string(props.Title());
            artist = winrt::to_string(props.Artist());
            album = winrt::to_string(props.AlbumTitle());
        }

        if (playbackInfo) {
            auto status = playbackInfo.PlaybackStatus();
            if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                trackState = "1";
            }
            else if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused) {
                trackState = "2";
            }
            else {
                trackState = "0";
            }
        }

        std::lock_guard<std::mutex> lock(serial_mutex);
        bool dataChanged = false;

        // Отправка измененных параметров с сохранением микропауз
        if (title != last_title) {
            sendTextPacket(hSerial_global, 0x01, title);
            last_title = title;
            dataChanged = true;
            Sleep(5); // Микропауза для стабильности USB-потока
        }
        if (artist != last_artist) {
            sendTextPacket(hSerial_global, 0x02, artist);
            last_artist = artist;
            dataChanged = true;
            Sleep(5);
        }
        if (album != last_album) {
            sendTextPacket(hSerial_global, 0x03, album);
            last_album = album;
            dataChanged = true;
            Sleep(5);
        }
        if (appAumid != last_app) {
            sendTextPacket(hSerial_global, 0x04, appAumid);
            last_app = appAumid;
            dataChanged = true;
            Sleep(5);
        }
        if (trackState != last_state) {
            sendTextPacket(hSerial_global, 0x07, trackState);
            last_state = trackState;
            dataChanged = true;
            Sleep(5);
        }

        if (dataChanged) {
            std::cout << "Updated -> Title: " << title << " | Artist: " << artist << " | State: " << trackState << "\n";
        }
    }
    catch (...) {
        // Защита от race conditions при закрытии/переключении сессий
    }
}

// Привязка к активной медиасессии и её событиям
void attachToActiveSession(GlobalSystemMediaTransportControlsSessionManager const& manager) {
    auto sessions = manager.GetSessions();

    {
        std::lock_guard<std::mutex> lock(session_mutex);

        // Отписываемся от предыдущей сессии, если она была
        if (current_session) {
            try {
                current_session.MediaPropertiesChanged(mediaPropsToken);
                current_session.PlaybackInfoChanged(playbackInfoToken);
            }
            catch (...) {}
            current_session = nullptr;
        }

        if (sessions.Size() > 0) {
            current_session = sessions.GetAt(0);

            // Подписываемся на события изменения метаданных трека
            mediaPropsToken = current_session.MediaPropertiesChanged([](auto const& sender, auto const&) {
                processAndSendSessionData(sender);
                });

            // Подписываемся на события изменения статуса воспроизведения (play/pause)
            playbackInfoToken = current_session.PlaybackInfoChanged([](auto const& sender, auto const&) {
                processAndSendSessionData(sender);
                });
        }
    }

    if (sessions.Size() > 0) {
        // Сразу синхронизируем состояние при подключении (вне мьютекса сессии)
        processAndSendSessionData(sessions.GetAt(0));
    }
    else {
        std::cout << "[-] No active media sessions found.\n";
    }
}

int main() {
    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);

    std::cout << "[*] Initializing WinRT apartment...\n";
    winrt::init_apartment();

    std::cout << "[*] Opening COM8 port...\n";

    HANDLE hSerial = CreateFileA(
        "\\\\.\\COM8",
        GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cerr << "[-] Error: Failed to open port COM8! Ensure the PlatformIO Serial Monitor is closed.\n";
        return 1;
    }

    hSerial_global = hSerial;

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);

    if (!GetCommState(hSerial, &dcbSerialParams)) {
        std::cerr << "[-] Error: Failed to get COM state.\n";
        CloseHandle(hSerial);
        return 1;
    }

    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;
    dcbSerialParams.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(hSerial, &dcbSerialParams)) {
        std::cerr << "[-] Error: Failed to set COM state.\n";
        CloseHandle(hSerial);
        return 1;
    }

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);

    std::cout << "[+] Port COM8 successfully opened and configured.\n";

    try {
        std::cout << "[*] Requesting Global System Media Transport Controls Session Manager...\n";
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        std::cout << "[+] Media session manager acquired successfully. Starting event-driven loop...\n\n";

        // Отслеживание появления/закрытия сессий плееров
        manager.SessionsChanged([manager](auto const&, auto const&) {
            attachToActiveSession(manager);
            });

        // Первичная привязка
        attachToActiveSession(manager);

        // Основные данные приходят по событиям WinRT,
        // а строка с временем трека обновляется раз в секунду
        while (true) {
            sendTimelineData();
            Sleep(1000);
        }
    }
    catch (const hresult_error& ex) {
        std::cerr << "[-] WinRT error: " << winrt::to_string(ex.message()) << "\n";
        CloseHandle(hSerial);
        return 1;
    }

    CloseHandle(hSerial);
    return 0;
}