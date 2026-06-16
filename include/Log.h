#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctime>

class Log {
public:
    static Log& get() {
        static Log instance;
        return instance;
    }

    // Call once at startup to enable logging to a file.
    // If never called, all LOG* macros are no-ops.
    void open(const std::string& path) {
        std::lock_guard lk(mu_);
        f_.open(path, std::ios::out | std::ios::trunc);
        if (!f_) throw std::runtime_error("Cannot open log file: " + path);
        enabled_ = true;
    }

    template<typename... Args>
    void write(int thread_id, const char* tag, Args&&... args) {
        if (!enabled_) return;

        std::ostringstream oss;
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        oss << std::put_time(std::localtime(&t), "%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << " [T" << thread_id << "] [" << tag << "] ";
        (oss << ... << std::forward<Args>(args));
        oss << '\n';

        std::lock_guard lk(mu_);
        f_ << oss.str();
        f_.flush();
    }

private:
    Log() = default;
    std::ofstream f_;
    std::mutex mu_;
    bool enabled_ = false;
};

#define LOG(tid, tag, ...) Log::get().write(tid, tag, __VA_ARGS__)
#define LOGI(tid, ...) Log::get().write(tid, "INFO", __VA_ARGS__)
#define LOGW(tid, ...) Log::get().write(tid, "WARN", __VA_ARGS__)
#define LOGE(tid, ...) Log::get().write(tid, "ERR ", __VA_ARGS__)
