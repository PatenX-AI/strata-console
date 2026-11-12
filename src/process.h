#pragma once
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "config_store.h"
#include "util.h"

class StatusCache {
public:
    void start();
    void stop();
    void set_target(int port, const std::string& key, const std::string& log_path);
    void set_history_path(const std::string& path);

    json state();
    std::string last_log();
    int port();

private:
    void loop();
    void load_history_locked();
    bool merge_history_locked(const json& metrics);
    void save_history_locked(bool force);
    json history_totals_locked() const;

    std::mutex m_;
    std::atomic<bool> running_{true};
    std::thread thread_;
    int port_ = 0;
    std::string key_;
    std::string log_path_;
    json health_ = json();
    json metrics_ = json();
    std::map<std::string, int> slot_by_request_;
    std::string history_path_;
    json history_ = json::array();
    int64_t history_saved_ = 0;
    std::string error_;
    std::string last_log_;
    int64_t updated_ = 0;
    int failures_ = 0;
};

class ProcessManager {
public:
    ProcessManager(ConfigStore& config, StatusCache& status);
    ~ProcessManager();

    bool start(bool lazy_override, bool is_restart, std::string& err);
    bool stop(bool unload_first, std::string& err);
    bool restart(bool lazy_override, std::string& err);
    bool load_model(std::string& err);
    bool unload_model(std::string& err);

    void tick();
    void start_monitor();
    void shutdown();

    json status();
    std::string log_path();

private:
    void monitor_loop();
    void reset_process_locked();

    ConfigStore& config_;
    StatusCache& status_;
    std::mutex m_;
    std::atomic<bool> running_{true};
    std::thread monitor_;

    HANDLE proc_ = nullptr;
    DWORD pid_ = 0;
    bool owned_ = false;
    bool intentional_ = false;
    std::string state_ = "stopped";
    std::string last_error_;
    DWORD exit_code_ = 0;
    int restarts_ = 0;
    int failures_ = 0;
    int backoff_idx_ = 0;
    bool auto_restart_ = false;
    bool lazy_on_restart_ = true;
    int active_port_ = 0;
    std::string active_config_;
    std::string active_log_;
    std::string command_;
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point next_restart_{};
    int64_t started_epoch_ = 0;
};
