#pragma once
#include "platform.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace evening {
using Json = nlohmann::json;
constexpr int ProtocolVersion = 1;
constexpr const char *ReleaseVersion = "1.1.0";
std::wstring ownerSid();
std::wstring runtimeIdentity(const std::filesystem::path &data);
std::wstring pipeName(const std::filesystem::path &data);
std::wstring quoteArgument(const std::wstring &value);
Json engineRequest(const std::filesystem::path &data, Json request, DWORD timeout = 3000);
int runEngine(const std::filesystem::path &data, bool simulate);
bool launchEngine(const std::filesystem::path &data, bool simulate, std::wstring &error);
bool backgroundTaskExists(const std::filesystem::path &data);
bool runBackgroundTask(const std::filesystem::path &data, std::wstring &error);
Json configureBackground(const std::filesystem::path &data, bool enabled, bool simulate);
class EngineConnection {
    std::atomic_bool stopping{false};
    std::thread worker;
    std::atomic_bool startRequested{false};
    std::atomic_bool finished{true};
    std::mutex pipeMutex;
    HANDLE stream = INVALID_HANDLE_VALUE;

  public:
    ~EngineConnection();
    void start(std::filesystem::path data, bool simulate, HWND window, UINT message);
    void stop();
    void requestStart() { startRequested = true; }
};
} // namespace evening
