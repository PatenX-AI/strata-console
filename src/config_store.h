#pragma once
#include <mutex>
#include <string>

#include "util.h"

class ConfigStore {
public:
    explicit ConfigStore(std::string app_dir);

    bool init(std::string& err);

    json configs() const;
    json models() const;
    json view() const;
    json launcher_public() const;

    bool select_config(const std::string& path, std::string& err);
    bool select_model(const std::string& id, std::string& err);
    bool set_data_dir(const std::string& path, std::string& err);
    bool merge_user_args_into_config(std::string& err);
    bool save_as(const std::string& name, std::string& err);
    bool apply(const json& form, std::string& err);

    bool start_preflight(std::string& err) const;
    json command_preview() const;
    std::string llama_swap_yaml() const;
    std::wstring build_command(bool force_lazy) const;

    json backups() const;
    bool restore_backup(const std::string& path, std::string& err);

    std::string api_key() const;
    std::string control_token() const;
    int model_port() const;
    int ui_port() const;
    std::string log_path() const;
    std::string root() const;
    std::string data_dir() const;
    std::string config_path() const;
    bool auto_restart() const;
    bool lazy_on_restart() const;

private:
    std::string app_dir_;
    std::string launcher_path_;
    mutable std::mutex m_;
    json launcher_ = json::object();
    json cfg_ = json::object();
    json saved_user_args_ = json::array();
    std::string config_path_;
    std::string root_;
    std::string data_dir_;

    json default_launcher() const;
    void load_launcher_locked();
    bool save_launcher_locked(std::string& err);
    std::string discover_root_locked() const;
    std::string discover_config_locked() const;
    std::string discover_data_dir_locked() const;
    void load_config_locked();
    json view_locked() const;
    json configs_locked() const;
    json models_locked() const;
    bool has_native_mtp_locked() const;
    bool select_model_locked(const std::string& id, std::string& err);
    bool materialize_model_config_locked(json& model, std::string& err);
    json backups_locked() const;
    std::string backup_dir_locked() const;
    std::string shared_path_locked() const;
    std::string detect_mtp_path_locked() const;
    json read_shared_locked() const;
    bool save_shared_locked(const json& shared, std::string& err);
    bool create_backup_locked(std::string& err);
    void prune_backups_locked();
    bool save_config_locked(std::string& err);
    std::string api_key_locked() const;
    int model_port_locked() const;
    std::string log_path_locked() const;
    bool vision_enabled_locked() const;
    std::string start_mode_locked() const;
    std::wstring build_command_locked(bool force_lazy) const;
};

// --- small JSON arg helpers (shared with the process manager) ---
std::optional<std::string> arg_value(const json& args, const std::string& flag);
bool has_flag(const json& args, const std::string& flag);
void set_arg(json& args, const std::string& flag, const std::string& value);
void remove_arg(json& args, const std::string& flag);
void set_flag(json& args, const std::string& flag, bool on);
