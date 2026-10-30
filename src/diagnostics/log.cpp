#include "log.h"

#include <chrono>
#include <ctime>
#include <fstream>

namespace astro {

Logger& Logger::Instance() {
    static Logger l;
    return l;
}

void Logger::Open(const std::filesystem::path& file) {
    std::lock_guard<std::mutex> g(mu_);
    if (open_ && path_ == file) return;
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    path_ = file;
    open_ = true;
}

void Logger::Close() {
    std::lock_guard<std::mutex> g(mu_);
    open_ = false;
}

void Logger::SetDebug(bool on) {
    std::lock_guard<std::mutex> g(mu_);
    debug_ = on;
}

std::filesystem::path Logger::Path() const {
    std::lock_guard<std::mutex> g(mu_);
    return path_;
}

void Logger::Write(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> g(mu_);
    if (!open_) return;
    std::ofstream out(path_, std::ios::app);
    if (!out) return;
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    out << ts << " [" << level << "] " << msg << '\n';
}

void Logger::Info(const std::string& m) { Write("INFO", m); }
void Logger::Warn(const std::string& m) { Write("WARN", m); }
void Logger::Error(const std::string& m) { Write("ERROR", m); }
void Logger::Debug(const std::string& m) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!debug_) return;
    }
    Write("DEBUG", m);
}

}  // namespace astro
