#pragma once

#include <string>
#include <mutex>
#include <iostream>

namespace SanskyStream {

class Logger {
public:
    enum class Level {
        Info,
        Warning,
        Error
    };

    static Logger& GetInstance();

    void Log(Level level, const std::string& message);

    // Convenience macros
    #define LOG_INFO(msg) SanskyStream::Logger::GetInstance().Log(SanskyStream::Logger::Level::Info, msg)
    #define LOG_WARN(msg) SanskyStream::Logger::GetInstance().Log(SanskyStream::Logger::Level::Warning, msg)
    #define LOG_ERROR(msg) SanskyStream::Logger::GetInstance().Log(SanskyStream::Logger::Level::Error, msg)

private:
    Logger() = default;
    ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex m_mutex;
};

} // namespace SanskyStream
