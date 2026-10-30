#pragma once
#include <filesystem>
#include <mutex>
#include <string>

namespace astro {

// Small thread-safe file logger. Debug lines are dropped unless debug is enabled.
class Logger {
public:
    static Logger& Instance();
    void Open(const std::filesystem::path& file);   // idempotent; appends
    void Close();
    void SetDebug(bool on);
    void Info(const std::string& msg);
    void Warn(const std::string& msg);
    void Error(const std::string& msg);
    void Debug(const std::string& msg);
    std::filesystem::path Path() const;

private:
    void Write(const char* level, const std::string& msg);
    mutable std::mutex mu_;
    std::filesystem::path path_;
    bool debug_ = false;
    bool open_ = false;
};

}  // namespace astro
