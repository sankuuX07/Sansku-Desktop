#include "Logger.h"
#include <windows.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace SanskyStream {

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

void Logger::Log(Level level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Get current time
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
    localtime_s(&bt, &in_time_t);
    std::stringstream ss;
    ss << std::put_time(&bt, "%Y-%m-%d %X");

    std::string levelStr;
    switch (level) {
        case Level::Info:    levelStr = "[INFO] "; break;
        case Level::Warning: levelStr = "[WARN] "; break;
        case Level::Error:   levelStr = "[ERROR]"; break;
    }

    std::string fullMessage = ss.str() + " " + levelStr + " " + message + "\n";
    
    // Output to console
    std::cout << fullMessage;
    
    // Output to debugger
    OutputDebugStringA(fullMessage.c_str());
}

} // namespace SanskyStream
