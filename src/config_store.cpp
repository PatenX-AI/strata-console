#include "config_store.h"

#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace {

std::string sval(const json& j, const char* k, const std::string& def = "") {
    if (!j.is_object()) return def;
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number()) return std::to_string(it->get<double>());
    return def;
}

int ival(const json& j, const char* k, int def = 0) {
    if (!j.is_object()) return def;
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return def;
    try {
        if (it->is_number_integer()) return it->get<int>();
        if (it->is_number()) return (int)it->get<double>();
        if (it->is_string()) return std::stoi(it->get<std::string>());
    } catch (...) {}
    return def;
}

bool bval(const json& j, const char* k, bool def = false) {
    if (!j.is_object()) return def;
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    return def;
}

double dval(const json& j, const char* k, double def = 0) {
    if (!j.is_object()) return def;
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return def;
    try {
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) return std::stod(it->get<std::string>());
    } catch (...) {}
    return def;
}

std::string list_text(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_array()) {
        std::vector<std::string> v;
        for (const auto& x : j) if (x.is_string()) v.push_back(x.get<std::string>());
        return util::join(v, ", ");
    }
    return {};
}

json text_list(const std::string& text) {
    json a = json::array();
    for (const auto& p : util::split(text, ',')) if (!p.empty()) a.push_back(p);
    return a;
}

std::string gpu_text(const json& cfg) {
    auto it = cfg.find("gpu");
    if (it == cfg.end()) it = cfg.find("gpus");
    if (it == cfg.end()) return {};
    if (it->is_array()) {
        std::vector<std::string> v;
        for (const auto& x : *it) if (x.is_number_integer()) v.push_back(std::to_string(x.get<int>()));
        return util::join(v, ",");
    }
    if (it->is_number_integer()) return std::to_string(it->get<int>());
    if (it->is_string()) return it->get<std::string>();
    return {};
}

json parse_gpu(const std::string& text) {
    json a = json::array();
    for (const auto& p : util::split(text, ',')) {
        if (p.empty()) continue;
        try { a.push_back(std::stoi(p)); } catch (...) {}
    }
    return a;
}

std::string filename_of(const std::string& p) {
    return util::u8(util::path(p).filename());
}

std::string stem_of(const std::string& p) {
    return util::u8(util::path(p).stem());
}

std::string parent_of(const std::string& p) {
    return util::u8(util::path(p).parent_path());
}

bool is_strata_root(const std::string& root) {
    return util::is_file(root + "\\serve\\server.py");
}

int64_t file_mtime_epoch(const std::string& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(util::path(p), ec);
    if (ec) return 0;
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (int64_t)std::chrono::system_clock::to_time_t(sctp);
}

std::string mtime_text(const std::string& p) {
    auto t = (std::time_t)file_mtime_epoch(p);
    if (!t) return {};
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32]{};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

void set_bool_or_erase(json& cfg, const char* key, bool value) {
    if (value) cfg[key] = true;
    else cfg.erase(key);
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string path_key(const std::string& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(util::path(p), ec);
    std::string s = ec ? p : util::u8(c);
    std::replace(s.begin(), s.end(), '/', '\\');
    return lower_ascii(s);
}

std::string canonical_dir(const std::string& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(util::path(p), ec);
    return ec ? p : util::u8(c);
}

uint64_t file_size_of(const std::string& p) {
    std::error_code ec;
    auto n = fs::file_size(util::path(p), ec);
    return ec ? 0 : (uint64_t)n;
}

std::string first_shard_name(const std::string& pack) {
    std::string err;
    auto j = util::read_json(pack + "\\experts.bin.src.json", err);
    if (!j || !j->is_object()) return {};
    auto it = j->find("shards");
    if (it == j->end() || !it->is_array() || it->empty()) return {};
    const auto& s = (*it)[0];
    return s.is_object() ? sval(s, "name") : std::string{};
}

std::string find_named_file(const std::string& data_dir, const std::string& name) {
    if (name.empty()) return {};
    const std::string want = lower_ascii(name);
    std::error_code ec;
    auto direct = util::path(data_dir) / util::path(name);
    if (util::is_file(util::u8(direct))) return util::u8(direct);

    for (auto& child : fs::directory_iterator(util::path(data_dir), ec)) {
        if (ec) break;
        if (!child.is_directory()) continue;
        std::string folder = lower_ascii(util::u8(child.path().filename()));
        if (folder.rfind("models", 0) != 0) continue;
        fs::recursive_directory_iterator it(
            child.path(), fs::directory_options::skip_permission_denied |
                          fs::directory_options::follow_directory_symlink, ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file()) continue;
            if (lower_ascii(util::u8(it->path().filename())) == want)
                return util::u8(it->path());
        }
    }
    return {};
}

std::string detect_quant(const std::string& text) {
    static const char* kQuants[] = {
        "IQ2_XXS", "IQ2_XS", "IQ2_S", "IQ3_XXS", "IQ3_XS", "IQ3_S", "IQ4_XS", "IQ4_NL",
        "UD-Q8_0", "UD-Q6_K_XL", "UD-Q5_K_XL", "UD-Q4_K_XL", "UD-Q4_K_M", "UD-Q4_K_S",
        "UD-Q3_K_XL", "UD-Q2_K_XL", "TQK6", "TQK7", "TQ2_0", "TQ1_0", "NVFP4", "FP4",
        "Q2_K", "Q2_0", "Q3_K", "Q4_K", "Q4_0", "Q5_K", "Q5_0", "Q6_K", "Q8_0",
    };
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    for (const char* q : kQuants) {
        if (upper.find(q) != std::string::npos) return q;
    }
    return {};
}

std::string pretty_model_name(const std::string& pack_name, const std::string& configured) {
    std::string p = lower_ascii(pack_name);
    if (p == "swift-iq2_xs" || p.rfind("swift-", 0) == 0) return "Swift 1.5";
    if (p.rfind("gyro-gyro-", 0) == 0) {
        std::string tail = pack_name.substr(std::string("gyro-gyro-").size());
        for (auto& c : tail) c = (char)std::toupper((unsigned char)c);
        return "Gyro-" + tail;
    }
    if (!configured.empty()) return configured;
    std::string out = pack_name;
    std::replace(out.begin(), out.end(), '_', ' ');
    std::replace(out.begin(), out.end(), '-', ' ');
    return out;
}

std::string pretty_native_name(const std::string& configured, const std::string& native,
                               const std::string& fallback) {
    std::string hay = lower_ascii(configured + " " + native);
    if (hay.find("qwen3.6") != std::string::npos && hay.find("35b") != std::string::npos)
        return "Qwen3.6-35B-A3B";
    if (!configured.empty()) return configured;
    return fallback;
}

std::string find_vision_exe(const std::string& root, bool gpu) {
    std::string main = root + "\\engine\\strata-vision.exe";
    std::error_code ec;
    if (gpu) {
        if (util::is_file(main) && file_size_of(main) > 10 * 1024 * 1024) return main;
        return {};
    }
    std::string cpu;
    for (auto& e : fs::directory_iterator(util::path(root + "\\engine"), ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string n = lower_ascii(util::u8(e.path().filename()));
        if (n.rfind("strata-vision-cpu", 0) == 0 && e.path().extension() == L".exe") {
            cpu = util::u8(e.path());
            break;
        }
    }
    if (!cpu.empty()) return cpu;
    if (util::is_file(main) && file_size_of(main) <= 10 * 1024 * 1024) return main;
    return {};
}

std::string yaml_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string safe_model_id(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out += (char)c;
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "model" : out;
}

}  // namespace

std::optional<std::string> arg_value(const json& args, const std::string& flag) {
    if (!args.is_array()) return std::nullopt;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i].is_string() && args[i].get<std::string>() == flag) {
            if (args[i + 1].is_string()) return args[i + 1].get<std::string>();
            if (args[i + 1].is_number_integer()) return std::to_string(args[i + 1].get<int>());
            if (args[i + 1].is_number()) return std::to_string(args[i + 1].get<double>());
        }
    }
    return std::nullopt;
}

bool has_flag(const json& args, const std::string& flag) {
    if (!args.is_array()) return false;
    for (const auto& x : args) if (x.is_string() && x.get<std::string>() == flag) return true;
    return false;
}

void set_arg(json& args, const std::string& flag, const std::string& value) {
    if (!args.is_array()) args = json::array();
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].is_string() && args[i].get<std::string>() == flag) {
            if (i + 1 < args.size()) args[i + 1] = value;
            else args.push_back(value);
            return;
        }
    }
    args.push_back(flag);
    args.push_back(value);
}

void remove_arg(json& args, const std::string& flag) {
    if (!args.is_array()) return;
    for (size_t i = 0; i < args.size();) {
        if (args[i].is_string() && args[i].get<std::string>() == flag) {
            size_t n = 1;
            while (i + n < args.size() &&
                   !(args[i + n].is_string() &&
                     args[i + n].get<std::string>().rfind("--", 0) == 0)) {
                ++n;
            }
            args.erase(args.begin() + i, args.begin() + i + n);
        } else {
            ++i;
        }
    }
}

void set_flag(json& args, const std::string& flag, bool on) {
    if (!args.is_array()) args = json::array();
    if (on) {
        if (!has_flag(args, flag)) args.push_back(flag);
    } else {
        for (size_t i = 0; i < args.size();) {
            if (args[i].is_string() && args[i].get<std::string>() == flag) args.erase(args.begin() + i);
            else ++i;
        }
    }
}

json update_user_args(const json& original, const json& effective) {
    json result = json::array();
    if (!original.is_array() || !effective.is_array()) return result;
    auto groups_of = [](const json& args) {
        std::vector<std::pair<std::string, json>> groups;
        json current = json::array();
        std::string flag;
        for (const auto& v : args) {
            if (!v.is_string()) continue;
            std::string s = v.get<std::string>();
            if (s.rfind("--", 0) == 0) {
                if (!current.empty()) groups.emplace_back(flag, current);
                current = json::array();
                flag = s;
            }
            current.push_back(s);
        }
        if (!current.empty()) groups.emplace_back(flag, current);
        return groups;
    };
    auto effective_groups = groups_of(effective);
    std::map<std::string, json> by_flag;
    for (const auto& g : effective_groups) if (!g.first.empty()) by_flag[g.first] = g.second;
    for (const auto& g : groups_of(original)) {
        if (g.first.empty()) continue;
        auto it = by_flag.find(g.first);
        if (it == by_flag.end()) continue;
        for (const auto& v : it->second) result.push_back(v);
    }
    return result;
}

void merge_user_args(json& cfg) {
    if (!cfg.is_object()) return;
    auto user_it = cfg.find("user_args");
    auto args_it = cfg.find("args");
    if (user_it == cfg.end() || !user_it->is_array() ||
        args_it == cfg.end() || !args_it->is_array()) {
        cfg.erase("user_args");
        return;
    }

    std::set<std::string> overridden;
    for (const auto& v : *user_it) {
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (s.rfind("--", 0) == 0) overridden.insert(s);
        }
    }

    json merged = json::array();
    const json& args = *args_it;
    for (size_t i = 0; i < args.size();) {
        size_t start = i;
        bool is_flag = args[i].is_string() &&
                       args[i].get<std::string>().rfind("--", 0) == 0;
        std::string flag = is_flag ? args[i].get<std::string>() : "";
        ++i;
        while (i < args.size()) {
            bool next_flag = args[i].is_string() &&
                             args[i].get<std::string>().rfind("--", 0) == 0;
            if (next_flag) break;
            ++i;
        }
        if (is_flag && overridden.count(flag)) continue;
        for (size_t k = start; k < i; ++k) merged.push_back(args[k]);
    }
    for (const auto& v : *user_it) merged.push_back(v);
    cfg["args"] = std::move(merged);
    cfg.erase("user_args");
}

ConfigStore::ConfigStore(std::string app_dir)
    : app_dir_(std::move(app_dir)),
      launcher_path_(app_dir_ + "\\strata-console.json") {}

json ConfigStore::default_launcher() const {
    return json{
        {"ui_port", 8090},
        {"config_path", ""},
        {"strata_root", ""},
        {"data_dir", ""},
        {"auto_restart", true},
        {"lazy_on_restart", true},
        {"backup_count", 5},
        {"control_token", util::random_hex(24)}
    };
}

void ConfigStore::load_launcher_locked() {
    launcher_ = default_launcher();
    std::string err;
    auto j = util::read_json(launcher_path_, err);
    if (j && j->is_object()) {
        for (auto it = j->begin(); it != j->end(); ++it) launcher_[it.key()] = it.value();
    }
    if (!launcher_.contains("control_token") || !launcher_["control_token"].is_string() ||
        launcher_["control_token"].get<std::string>().empty()) {
        launcher_["control_token"] = util::random_hex(24);
    }
    launcher_["backup_count"] = std::clamp(ival(launcher_, "backup_count", 5), 1, 50);
    launcher_["ui_port"] = std::clamp(ival(launcher_, "ui_port", 8090), 1024, 65535);
}

bool ConfigStore::save_launcher_locked(std::string& err) {
    return util::write_json_atomic(launcher_path_, launcher_, err);
}

std::string ConfigStore::discover_root_locked() const {
    std::string from_launcher = sval(launcher_, "strata_root");
    if (is_strata_root(from_launcher)) return from_launcher;

    std::string appdata = util::get_env_utf8(L"APPDATA");
    if (!appdata.empty()) {
        std::string err;
        auto settings = util::read_json(appdata + "\\Strata\\settings.json", err);
        if (settings && settings->is_object()) {
            auto it = settings->find("installs");
            if (it != settings->end() && it->is_array()) {
                for (const auto& x : *it) {
                    if (x.is_string() && is_strata_root(x.get<std::string>()))
                        return x.get<std::string>();
                }
            }
        }
    }
    std::vector<std::string> candidates;
    candidates.push_back(util::u8(util::path(app_dir_).parent_path() / L"Strata"));
    candidates.push_back(app_dir_ + "\\Strata");
    for (const wchar_t* var : {L"USERPROFILE", L"LOCALAPPDATA", L"ProgramData"}) {
        std::string base = util::get_env_utf8(var);
        if (!base.empty()) candidates.push_back(base + "\\Strata");
    }
    for (const auto& p : candidates) if (is_strata_root(p)) return p;
    return app_dir_;
}

std::string ConfigStore::discover_data_dir_locked() const {
    std::string saved = sval(launcher_, "data_dir");
    if (util::is_dir(saved)) return canonical_dir(saved);

    std::string appdata = util::get_env_utf8(L"APPDATA");
    if (!appdata.empty()) {
        std::string err;
        auto settings = util::read_json(appdata + "\\Strata\\settings.json", err);
        if (settings && settings->is_object()) {
            std::string d = sval(*settings, "data_dir");
            if (util::is_dir(d)) return canonical_dir(d);
        }
    }

    std::vector<std::string> candidates;
    if (!root_.empty()) {
        candidates.push_back(util::u8(util::path(root_).parent_path() / L"Strata-data"));
        candidates.push_back(root_ + "\\Strata-data");
    }
    for (const auto& p : candidates) if (util::is_dir(p)) return canonical_dir(p);
    return candidates.empty() ? std::string{} : candidates.front();
}

std::string ConfigStore::discover_config_locked() const {
    std::string saved = sval(launcher_, "config_path");
    if (util::is_file(saved)) return saved;
    std::string best;
    int64_t best_time = -1;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(util::path(root_), ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string name = util::u8(e.path().filename());
        if (name.rfind("strata-", 0) != 0) continue;
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
        int64_t t = file_mtime_epoch(util::u8(e.path()));
        if (t > best_time) { best_time = t; best = util::u8(e.path()); }
    }
    return best;
}

void ConfigStore::load_config_locked() {
    if (config_path_.empty()) { cfg_ = json::object(); return; }
    std::string err;
    auto j = util::read_json(config_path_, err);
    if (!j || !j->is_object()) {
        cfg_ = json::object();
        return;
    }
    cfg_ = *j;
    if (!cfg_.contains("args") || !cfg_["args"].is_array()) cfg_["args"] = json::array();
    saved_user_args_ = cfg_.value("user_args", json::array());
    merge_user_args(cfg_);
}

bool ConfigStore::init(std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    std::string launcher_err;
    (void)launcher_err;
    load_launcher_locked();
    root_ = discover_root_locked();
    data_dir_ = discover_data_dir_locked();
    config_path_ = discover_config_locked();
    if (root_.empty()) root_ = app_dir_;
    load_config_locked();
    if (config_path_.empty()) {
        err = "no strata-*.json found in " + root_;
        return false;
    }
    return true;
}

json ConfigStore::configs_locked() const {
    json out = json::array();
    std::error_code ec;
    if (root_.empty()) return out;
    for (auto& e : fs::directory_iterator(util::path(root_), ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string name = util::u8(e.path().filename());
        if (name.rfind("strata-", 0) != 0) continue;
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
        std::string p = util::u8(e.path());
        std::string model;
        std::string read_err;
        auto j = util::read_json(p, read_err);
        if (j && j->is_object()) model = sval(*j, "model_name");
        out.push_back(json{{"path", p}, {"name", name}, {"model_name", model},
                           {"mtime", file_mtime_epoch(p)}, {"mtime_text", mtime_text(p)}});
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return a.value("mtime", 0) > b.value("mtime", 0);
    });
    return out;
}

json ConfigStore::configs() const {
    std::lock_guard<std::mutex> lk(m_);
    return configs_locked();
}

json ConfigStore::models_locked() const {
    json out = json::array();
    if (data_dir_.empty()) return out;

    struct CfgInfo {
        std::string path, model_name, native, pack, tokenizer, mmproj;
        int context = 0;
    };
    std::vector<CfgInfo> configs;
    std::map<std::string, std::vector<size_t>> by_pack;
    std::error_code ec;

    for (auto& e : fs::directory_iterator(util::path(root_), ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string name = util::u8(e.path().filename());
        if (name.rfind("strata-", 0) != 0) continue;
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
        std::string p = util::u8(e.path());
        std::string read_err;
        auto j = util::read_json(p, read_err);
        if (!j || !j->is_object()) continue;
        auto args = j->value("args", json::array());
        CfgInfo ci;
        ci.path = p;
        ci.model_name = sval(*j, "model_name");
        ci.pack = arg_value(args, "--pack").value_or("");
        ci.native = arg_value(args, "--native").value_or("");
        ci.tokenizer = sval(*j, "tokenizer");
        ci.context = [&] {
            try { return std::stoi(arg_value(args, "--max-context").value_or("0")); }
            catch (...) { return 0; }
        }();
        if (j->contains("vision") && (*j)["vision"].is_object())
            ci.mmproj = sval((*j)["vision"], "mmproj");
        configs.push_back(ci);
        if (!ci.pack.empty()) {
            by_pack[path_key(ci.pack)].push_back(configs.size() - 1);
        }
    }

    auto find_mmproj = [](const std::string& dir) -> std::string {
        std::error_code e;
        for (auto& x : fs::directory_iterator(util::path(dir), e)) {
            if (e) break;
            if (!x.is_regular_file()) continue;
            std::string n = lower_ascii(util::u8(x.path().filename()));
            if (n.rfind("mmproj", 0) == 0 && x.path().extension() == L".gguf")
                return util::u8(x.path());
        }
        return {};
    };

    auto add_model = [&](const std::string& pack, const CfgInfo* ci, bool multi) {
        std::string pack_name = util::u8(util::path(pack).filename());
        std::string shard = first_shard_name(pack);
        std::string native = ci ? ci->native : "";
        if (native.empty() && !shard.empty()) native = find_named_file(data_dir_, shard);
        std::string tokenizer = ci ? ci->tokenizer : "";
        if (tokenizer.empty()) {
            std::string t = pack + "\\tokenizer";
            if (util::is_dir(t)) tokenizer = t;
        }
        std::string mmproj = ci ? ci->mmproj : "";
        if (mmproj.empty() && !native.empty()) mmproj = find_mmproj(parent_of(native));

        std::string configured = ci ? ci->model_name : "";
        std::string id = safe_model_id(pack_name);
        if (multi && ci) {
            std::string stem = filename_of(ci->path);
            if (stem.rfind("strata-", 0) == 0) stem = stem.substr(7);
            if (stem.size() > 5 && stem.substr(stem.size() - 5) == ".json")
                stem = stem.substr(0, stem.size() - 5);
            id = safe_model_id(stem);
        }
        std::string display = multi && !configured.empty()
            ? configured : pretty_model_name(pack_name, configured);
        std::string quant = detect_quant(util::u8(util::path(native).filename()) + " " + pack_name);
        bool native_ok = !native.empty() && util::is_file(native);
        bool tokenizer_ok = !tokenizer.empty() && util::is_dir(tokenizer);
        uint64_t bytes = file_size_of(pack + "\\experts.bin") + file_size_of(pack + "\\dense.bin");
        std::string reason;
        if (native.empty()) reason = "缺少 GGUF 首分片";
        else if (!native_ok) reason = "GGUF 首分片不存在";
        else if (!tokenizer_ok) reason = "缺少 tokenizer";
        json m{
            {"id", id},
            {"name", display},
            {"model_name", configured.empty() ? id : configured},
            {"quant", quant},
            {"pack", pack},
            {"native", native},
            {"tokenizer", tokenizer},
            {"mmproj", mmproj},
            {"config", ci ? ci->path : ""},
            {"context", ci && ci->context > 0 ? ci->context : 131072},
            {"size_bytes", bytes},
            {"installed", native_ok && tokenizer_ok},
            {"tokenizer_ok", tokenizer_ok},
            {"native_only", false},
            {"active", ci && !ci->path.empty() && path_key(ci->path) == path_key(config_path_)},
            {"reason", reason},
        };
        out.push_back(std::move(m));
    };

    std::set<std::string> seen_packs;
    std::set<std::string> seen_configs;
    std::string packs_dir = data_dir_ + "\\packs";
    if (util::is_dir(packs_dir)) {
        for (auto& e : fs::directory_iterator(util::path(packs_dir), ec)) {
            if (ec) break;
            if (!e.is_directory()) continue;
            std::string pack = util::u8(e.path());
            std::string key = path_key(pack);
            seen_packs.insert(key);
            auto it = by_pack.find(key);
            if (it == by_pack.end()) {
                if (util::is_file(pack + "\\experts.bin")) add_model(pack, nullptr, false);
            } else {
                bool multi = it->second.size() > 1;
                for (size_t idx : it->second) {
                    seen_configs.insert(path_key(configs[idx].path));
                    add_model(pack, &configs[idx], multi);
                }
            }
        }
    }

    // GGUF-native models (for example Qwen3.6-35B-A3B) need no experts pack.
    for (const auto& ci : configs) {
        if (ci.path.empty() || seen_configs.count(path_key(ci.path))) continue;
        if (ci.native.empty() || !util::is_file(ci.native)) continue;
        if (!ci.pack.empty() && seen_packs.count(path_key(ci.pack))) continue;

        std::string stem = filename_of(ci.path);
        if (stem.rfind("strata-", 0) == 0) stem = stem.substr(7);
        if (stem.size() > 5 && stem.substr(stem.size() - 5) == ".json")
            stem = stem.substr(0, stem.size() - 5);
        std::string id = safe_model_id(stem);
        std::string tokenizer = ci.tokenizer;
        if (tokenizer.empty()) {
            std::string t = data_dir_ + "\\packs\\" + stem + "\\tokenizer";
            if (util::is_dir(t)) tokenizer = t;
        }
        std::string mmproj = ci.mmproj;
        if (mmproj.empty()) mmproj = find_mmproj(parent_of(ci.native));
        bool tokenizer_ok = !tokenizer.empty() && util::is_dir(tokenizer);
        std::string reason = tokenizer_ok ? "" : "缺少 tokenizer";
        out.push_back(json{
            {"id", id},
            {"name", pretty_native_name(ci.model_name, ci.native, id)},
            {"model_name", ci.model_name.empty() ? id : ci.model_name},
            {"quant", detect_quant(util::u8(util::path(ci.native).filename()) + " " + ci.model_name)},
            {"pack", ci.pack},
            {"native", ci.native},
            {"tokenizer", tokenizer},
            {"mmproj", mmproj},
            {"config", ci.path},
            {"context", ci.context > 0 ? ci.context : 32768},
            {"size_bytes", file_size_of(ci.native)},
            {"installed", tokenizer_ok},
            {"tokenizer_ok", tokenizer_ok},
            {"native_only", true},
            {"active", path_key(ci.path) == path_key(config_path_)},
            {"reason", reason},
        });
    }

    std::set<std::string> ids;
    for (auto& m : out) {
        std::string base = m.value("id", "model");
        std::string id = base;
        for (int i = 2; ids.count(id); ++i) id = base + "-" + std::to_string(i);
        ids.insert(id);
        m["id"] = id;
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        if (a.value("active", false) != b.value("active", false))
            return a.value("active", false);
        return a.value("name", "") < b.value("name", "");
    });
    return out;
}

json ConfigStore::models() const {
    std::lock_guard<std::mutex> lk(m_);
    return models_locked();
}

std::string ConfigStore::api_key_locked() const {
    auto is_none = [](const std::string& s) {
        std::string v = util::trim(s);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return v == "none";
    };
    auto it = cfg_.find("api_key");
    if (it == cfg_.end() || it->is_null()) return {};
    if (it->is_string()) {
        std::string v = it->get<std::string>();
        return is_none(v) ? "" : v;
    }
    if (it->is_array()) {
        if (it->size() == 1 && (*it)[0].is_string() && is_none((*it)[0].get<std::string>()))
            return {};
        for (const auto& x : *it) if (x.is_string() && !x.get<std::string>().empty())
            return x.get<std::string>();
    }
    return {};
}

std::string ConfigStore::api_key() const {
    std::lock_guard<std::mutex> lk(m_);
    return api_key_locked();
}

int ConfigStore::model_port_locked() const {
    return ival(cfg_, "port", 8080);
}

int ConfigStore::model_port() const {
    std::lock_guard<std::mutex> lk(m_);
    return model_port_locked();
}

int ConfigStore::ui_port() const {
    std::lock_guard<std::mutex> lk(m_);
    return ival(launcher_, "ui_port", 8090);
}

std::string ConfigStore::log_path_locked() const {
    std::string p = sval(cfg_, "log");
    if (!p.empty()) return p;
    if (!root_.empty()) return root_ + "\\strata-console-server.log";
    return {};
}

std::string ConfigStore::log_path() const {
    std::lock_guard<std::mutex> lk(m_);
    return log_path_locked();
}

std::string ConfigStore::root() const {
    std::lock_guard<std::mutex> lk(m_);
    return root_;
}

std::string ConfigStore::data_dir() const {
    std::lock_guard<std::mutex> lk(m_);
    return data_dir_;
}

std::string ConfigStore::config_path() const {
    std::lock_guard<std::mutex> lk(m_);
    return config_path_;
}

std::string ConfigStore::control_token() const {
    std::lock_guard<std::mutex> lk(m_);
    return sval(launcher_, "control_token");
}

bool ConfigStore::vision_enabled_locked() const {
    auto it = cfg_.find("vision");
    if (it != cfg_.end() && it->is_object() && !it->empty()) return true;
    return has_flag(cfg_.value("args", json::array()), "--vision");
}

std::string ConfigStore::start_mode_locked() const {
    return bval(cfg_, "lazy_load", false) ? "lazy" : "preload";
}

bool ConfigStore::has_native_mtp_locked() const {
    json args = cfg_.contains("args") && cfg_["args"].is_array() ? cfg_["args"] : json::array();
    std::string mtp = arg_value(args, "--mtp").value_or("");
    if (!mtp.empty() && std::all_of(mtp.begin(), mtp.end(),
                                    [](unsigned char c) { return std::isdigit(c) != 0; }))
        return true;
    std::string hay = lower_ascii(
        sval(cfg_, "model_name") + " " + sval(cfg_, "exe") + " " + sval(cfg_, "model_arch") +
        " " + arg_value(args, "--native").value_or("") + " " +
        arg_value(args, "--pack").value_or(""));
    return hay.find("qwen3.6") != std::string::npos ||
           hay.find("qwen3_6") != std::string::npos ||
           hay.find("qwen36") != std::string::npos ||
           hay.find("strata-q36") != std::string::npos;
}

json ConfigStore::view_locked() const {
    json j = json::object();
    j["config_path"] = config_path_;
    j["config_name"] = config_path_.empty() ? "" : filename_of(config_path_);
    j["strata_root"] = root_;
    j["data_dir"] = data_dir_;
    j["model_name"] = sval(cfg_, "model_name");
    j["aliases"] = list_text(cfg_.value("aliases", json()));
    j["user_args"] = saved_user_args_.is_array() ? saved_user_args_ : json::array();
    j["exe"] = sval(cfg_, "exe");
    j["tokenizer"] = sval(cfg_, "tokenizer");
    j["log"] = log_path_locked();
    j["host"] = sval(cfg_, "host", "127.0.0.1");
    j["port"] = model_port_locked();
    {
        std::string key = api_key_locked();
        j["api_key_set"] = !key.empty();
        j["api_key_prefix"] = key.empty() ? "" :
            (key.size() > 14 ? key.substr(0, 14) + "…" : key.substr(0, std::min<size_t>(4, key.size())) + "…");
        auto raw = cfg_.value("api_key", json());
        bool disabled = false;
        if (raw.is_string()) {
            std::string v = util::trim(raw.get<std::string>());
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            disabled = v == "none";
        }
        j["api_key_disabled"] = disabled;
    }
    j["cors_origins"] = list_text(cfg_.value("cors_origins", json()));
    j["trusted_origins"] = list_text(cfg_.value("trusted_origins", json()));
    j["allowed_hosts"] = list_text(cfg_.value("allowed_hosts", json()));
    j["idle_unload_min"] = cfg_.contains("idle_unload_s")
        ? dval(cfg_, "idle_unload_s", 0.0) / 60.0 : 60.0;
    j["open_browser"] = bval(cfg_, "open_browser", false);
    j["prevent_sleep"] = bval(cfg_, "prevent_sleep", false);
    j["api_monitor"] = bval(cfg_, "api_monitor", false);
    j["start_mode"] = start_mode_locked();
    j["gpu"] = gpu_text(cfg_);
    {
        auto roles = cfg_.value("gpu_roles", json());
        bool active = (roles.is_object() && !roles.empty()) || (roles.is_array() && !roles.empty());
        j["gpu_roles"] = roles;
        j["gpu_roles_active"] = active;
        std::string text;
        if (roles.is_object()) {
            for (auto it = roles.begin(); it != roles.end(); ++it) {
                if (!text.empty()) text += " · ";
                text += "GPU " + it.key() + "=" + (it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
            }
        } else if (roles.is_string()) {
            text = roles.get<std::string>();
        }
        j["gpu_roles_text"] = text;
    }
    {
        std::string build_path = root_ + "\\engine\\BUILD.json";
        std::string build_err;
        auto build = util::read_json(build_path, build_err);
        std::string engine_version = build && build->is_object()
            ? sval(*build, "version") : "";
        j["engine_version"] = engine_version;
        int maj = 0, min = 0, patch = 0;
        if (!engine_version.empty()) {
            sscanf_s(engine_version.c_str(), "%d.%d.%d", &maj, &min, &patch);
        }
        j["engine_version_ok"] = engine_version.empty() ||
            (maj > 0) || (maj == 0 && (min > 1 || (min == 1 && patch >= 46)));
    }
    j["reasoning_effort"] = sval(cfg_, "reasoning_effort");
    j["reasoning_budget_tokens"] = cfg_.contains("reasoning_budget_tokens")
        ? cfg_["reasoning_budget_tokens"] : json();
    j["answer_room"] = cfg_.contains("answer_room") ? bval(cfg_, "answer_room", true) : true;
    j["reasoning_close_retry"] = bval(cfg_, "reasoning_close_retry", false);
    {
        std::string loop = "off";
        auto it = cfg_.find("reasoning_loop_recovery");
        if (it != cfg_.end()) {
            if (it->is_string()) loop = it->get<std::string>();
            else if (it->is_boolean() && it->get<bool>()) loop = "recover";
        }
        j["reasoning_loop_recovery"] = loop;
    }
    j["repeat_stop_tokens"] = cfg_.contains("repeat_stop_tokens")
        ? cfg_["repeat_stop_tokens"] : 256;
    j["tool_call_recovery"] = cfg_.contains("tool_call_recovery")
        ? bval(cfg_, "tool_call_recovery", true) : true;
    j["effort_position"] = sval(cfg_, "effort_position", "start");
    j["anthropic_thinking"] = sval(cfg_, "anthropic_thinking", "model");
    j["codex_thread_titles"] = bval(cfg_, "codex_thread_titles", false);
    j["codex_compaction_cache"] = bval(cfg_, "codex_compaction_cache", false);
    {
        auto s = cfg_.value("sampling", json::object());
        if (!s.is_object()) s = json::object();
        j["temperature"] = s.contains("temperature") ? s["temperature"] : json();
        j["top_p"] = s.contains("top_p") ? s["top_p"] : json();
        j["top_k"] = s.contains("top_k") ? s["top_k"] : json();
        j["min_p"] = s.contains("min_p") ? s["min_p"] : json();
        j["presence_penalty"] = s.contains("presence_penalty") ? s["presence_penalty"] : json();
        j["frequency_penalty"] = s.contains("frequency_penalty") ? s["frequency_penalty"] : json();
        j["repetition_penalty"] = s.contains("repetition_penalty") ? s["repetition_penalty"] : json();
        j["penalty_last_n"] = s.contains("penalty_last_n") ? s["penalty_last_n"] : json();
        j["seed"] = s.contains("seed") ? s["seed"] : json();
    }
    {
        auto shared = read_shared_locked();
        j["max_tokens"] = shared.contains("max_tokens") ? shared["max_tokens"] : json(0);
    }
    j["fit_max_tokens"] = bval(cfg_, "fit_max_tokens", false);

    auto args = cfg_.value("args", json::array());
    j["native"] = arg_value(args, "--native").value_or("");
    j["ple"] = arg_value(args, "--ple-gguf").value_or("");
    j["pack"] = arg_value(args, "--pack").value_or("");
    j["expert_profile"] = arg_value(args, "--expert-profile").value_or("");
    auto arg_int = [&](const char* flag, int fallback = 0) {
        try { return std::stoi(arg_value(args, flag).value_or(std::to_string(fallback))); }
        catch (...) { return fallback; }
    };
    j["context"] = arg_int("--max-context", 0);
    j["kv"] = arg_value(args, "--kv").value_or("int8");
    {
        std::string mtp_value = arg_value(args, "--mtp").value_or("");
        bool mtp_count = !mtp_value.empty() &&
                         std::all_of(mtp_value.begin(), mtp_value.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; });
        std::string detected = detect_mtp_path_locked();
        std::string check_path = (!mtp_value.empty() && !mtp_count) ? mtp_value : detected;
        std::string draft_mode = sval(cfg_, "drafting", "");
        j["mtp"] = draft_mode != "off" && (!mtp_value.empty() || draft_mode == "mtp" || draft_mode == "dflash2");
        j["mtp_value"] = mtp_value;
        j["mtp_kind"] = mtp_value.empty() ? "" : (mtp_count ? "count" : "path");
        j["mtp_path"] = mtp_count ? "" : mtp_value;
        j["mtp_detected_path"] = detected;
        j["mtp_native"] = mtp_count || has_native_mtp_locked();
        j["mtp_available"] = mtp_count ||
            (!check_path.empty() && util::is_file(check_path + "\\experts.bin"));
    }
    j["drafting"] = sval(cfg_, "drafting", "auto");
    j["spec"] = arg_value(args, "--spec").value_or("");
    j["spec_min_p"] = arg_value(args, "--spec-min-p").value_or("");
    j["prefill"] = arg_value(args, "--prefill").value_or("");
    j["expert_cache"] = arg_value(args, "--expert-cache").value_or("");
    j["kv_resident"] = arg_value(args, "--kv-resident").value_or("");
    j["vram_reserve"] = arg_int("--vram-reserve-mib", 1024);
    j["vram_elastic"] = bval(cfg_, "vram_elastic", false);
    j["vram_segment_mib"] = cfg_.contains("vram_segment_mib") ? cfg_["vram_segment_mib"] : json();
    j["expert_profile_save"] = sval(cfg_, "expert_profile_save");
    j["expert_profile_save_every"] = cfg_.contains("expert_profile_save_every")
        ? cfg_["expert_profile_save_every"] : json();
    j["resident_budget_gib"] = arg_value(args, "--resident-budget-gib").value_or("");
    j["pool_workers"] = arg_value(args, "--pool-workers").value_or("");
    j["ple_io"] = arg_value(args, "--ple-io").value_or("");
    j["conversation_cache_mib"] = arg_value(args, "--conversation-cache-mib").value_or("");
    j["conversation_cache_slots"] = arg_value(args, "--conversation-cache-slots").value_or("");
    j["conversation_cache_min_free_mib"] = arg_value(args, "--conversation-cache-min-free-mib").value_or("");
    j["conversation_cache_min_tokens"] = arg_value(args, "--conversation-cache-min-tokens").value_or("");
    j["conversation_cache_park_gain"] = arg_value(args, "--conversation-cache-park-gain").value_or("");
    j["parallel"] = cfg_.contains("parallel") ? cfg_["parallel"] : 1;
    j["max_queue"] = cfg_.contains("max_queue") ? cfg_["max_queue"] : json();
    j["queue_timeout"] = cfg_.contains("queue_timeout") ? cfg_["queue_timeout"] : json();
    j["engine_ready_s"] = cfg_.contains("engine_ready_s") ? cfg_["engine_ready_s"] : json();

    bool resident_experts = has_flag(args, "--resident-experts");
    bool budget_experts = arg_value(args, "--resident-budget-gib").has_value();
    std::string low_ram = resident_experts ? "resident" :
                          budget_experts ? "budget" :
                          has_flag(args, "--mmap-experts") ? "mmap" : "off";
    j["low_ram"] = low_ram;
    j["vision"] = vision_enabled_locked();
    if (cfg_.contains("vision") && cfg_["vision"].is_object()) {
        j["vision_exe"] = sval(cfg_["vision"], "exe");
        j["vision_mmproj"] = sval(cfg_["vision"], "mmproj");
        j["vision_gpu"] = bval(cfg_["vision"], "gpu", false);
        j["vision_max_tokens"] = ival(cfg_["vision"], "max_tokens", 768);
        j["vision_threads"] = ival(cfg_["vision"], "threads", 8);
        bool gpu = bval(cfg_["vision"], "gpu", false);
        j["vision_elastic"] = cfg_["vision"].contains("elastic")
            ? bval(cfg_["vision"], "elastic", gpu) : gpu;
        j["vision_elastic_idle_s"] = ival(cfg_["vision"], "elastic_idle_s", 120);
        j["vision_elastic_mib"] = ival(cfg_["vision"], "elastic_mib", 2048);
        j["vision_min_tokens"] = ival(cfg_["vision"], "min_tokens", 0);
    } else {
        j["vision_exe"] = "";
        j["vision_mmproj"] = "";
        j["vision_gpu"] = false;
        j["vision_max_tokens"] = 768;
        j["vision_threads"] = 8;
        j["vision_elastic"] = false;
        j["vision_elastic_idle_s"] = 120;
        j["vision_elastic_mib"] = 2048;
        j["vision_min_tokens"] = 0;
    }
    j["vision_exe_gpu"] = find_vision_exe(root_, true);
    j["vision_exe_cpu"] = find_vision_exe(root_, false);

    json lan_urls = json::array();
    for (const auto& ip : util::local_ipv4s()) lan_urls.push_back("http://" + ip);
    j["lan_urls"] = lan_urls;

    j["auto_restart"] = bval(launcher_, "auto_restart", true);
    j["lazy_on_restart"] = bval(launcher_, "lazy_on_restart", true);
    j["backup_count"] = ival(launcher_, "backup_count", 5);
    j["ui_port"] = ival(launcher_, "ui_port", 8090);

    json warnings = json::array();
    if (!j["engine_version_ok"].get<bool>()) {
        warnings.push_back("当前引擎 " + j["engine_version"].get<std::string>() +
                           " 低于 Strata 0.2.x 建议的 0.1.46；GPU 角色等新功能可能不可用。");
    }
    if (j["host"].get<std::string>() != "127.0.0.1" &&
        j["host"].get<std::string>() != "localhost" &&
        j["host"].get<std::string>() != "::1" &&
        !j["api_key_set"].get<bool>()) {
        warnings.push_back("模型 API 监听在非本机地址，但还没有设置 API Key。");
    }
    if (j["vision"].get<bool>() &&
        (!util::is_file(j["vision_exe"].get<std::string>()) ||
         !util::is_file(j["vision_mmproj"].get<std::string>()))) {
        warnings.push_back("图片已开启，但视觉编码器或 mmproj 不存在。");
    }
    j["warnings"] = warnings;
    return j;
}

json ConfigStore::view() const {
    std::lock_guard<std::mutex> lk(m_);
    return view_locked();
}

json ConfigStore::launcher_public() const {
    std::lock_guard<std::mutex> lk(m_);
    return json{{"ui_port", ival(launcher_, "ui_port", 8090)}};
}

bool ConfigStore::select_config(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (!util::is_file(path)) { err = "配置文件不存在: " + path; return false; }
    std::string read_err;
    auto j = util::read_json(path, read_err);
    if (!j || !j->is_object()) { err = "配置文件不是有效的 JSON: " + read_err; return false; }
    config_path_ = path;
    cfg_ = *j;
    if (!cfg_.contains("args") || !cfg_["args"].is_array()) cfg_["args"] = json::array();
    saved_user_args_ = cfg_.value("user_args", json::array());
    merge_user_args(cfg_);
    launcher_["config_path"] = config_path_;
    launcher_["strata_root"] = root_;
    std::string save_err;
    if (!save_launcher_locked(save_err)) { err = save_err; return false; }
    return true;
}

bool ConfigStore::materialize_model_config_locked(json& model, std::string& err) {
    std::string native = sval(model, "native");
    std::string pack = sval(model, "pack");
    std::string id = safe_model_id(sval(model, "id", "model"));
    if (!util::is_file(native)) { err = "模型 GGUF 首分片不存在: " + native; return false; }
    if (!util::is_dir(pack)) { err = "模型 Pack 目录不存在: " + pack; return false; }

    std::string config_path = root_ + "\\strata-" + id + ".json";
    if (util::is_file(config_path)) {
        model["config"] = config_path;
        return true;
    }

    json cfg = cfg_.is_object() ? cfg_ : json::object();
    json args = cfg.value("args", json::array());
    if (!args.is_array()) args = json::array();
    remove_arg(args, "--pack");
    remove_arg(args, "--native");
    remove_arg(args, "--ple-gguf");
    remove_arg(args, "--mtp");
    remove_arg(args, "--vision");
    set_arg(args, "--pack", pack);
    set_arg(args, "--native", native);
    set_arg(args, "--ple-gguf", native);
    set_arg(args, "--max-context", std::to_string(ival(model, "context", 131072)));
    if (!arg_value(args, "--kv")) set_arg(args, "--kv", "int8");
    if (!arg_value(args, "--expert-cache")) set_arg(args, "--expert-cache", "auto");
    if (!arg_value(args, "--prefill")) set_arg(args, "--prefill", "auto");
    if (!arg_value(args, "--spec")) set_arg(args, "--spec", "4");
    if (!arg_value(args, "--spec-min-p")) set_arg(args, "--spec-min-p", "0.5");
    if (!arg_value(args, "--vram-reserve-mib")) set_arg(args, "--vram-reserve-mib", "1500");
    std::string profile = root_ + "\\data\\expert-profile.bin";
    if (util::is_file(profile)) set_arg(args, "--expert-profile", profile);
    std::string family = lower_ascii(sval(model, "model_name") + " " +
                                     sval(model, "native") + " " + sval(model, "pack"));
    bool native_mtp = family.find("qwen3.6") != std::string::npos ||
                      family.find("qwen3_6") != std::string::npos ||
                      family.find("qwen36") != std::string::npos;
    if (native_mtp) {
        set_arg(args, "--mtp", "2");
        remove_arg(args, "--spec");
        remove_arg(args, "--spec-min-p");
    } else {
        std::string mtp = data_dir_ + "\\mtp\\rt";
        if (util::is_file(mtp + "\\experts.bin")) set_arg(args, "--mtp", mtp);
    }
    if (!has_flag(args, "--mmap-experts") && !has_flag(args, "--resident-experts"))
        set_flag(args, "--mmap-experts", true);

    cfg["model_name"] = sval(model, "model_name", id);
    cfg["exe"] = root_ + "\\engine\\strata.exe";
    cfg["cwd"] = root_;
    cfg["log"] = root_ + "\\strata-" + id + ".log";
    cfg["host"] = sval(cfg, "host", "127.0.0.1");
    cfg["port"] = ival(cfg, "port", 12345);
    cfg["tokenizer"] = sval(model, "tokenizer");
    cfg["args"] = args;

    std::string mmproj = sval(model, "mmproj");
    if (!mmproj.empty() && util::is_file(mmproj)) {
        json v = cfg.contains("vision") && cfg["vision"].is_object()
                     ? cfg["vision"] : json::object();
        v["exe"] = root_ + "\\engine\\strata-vision.exe";
        v["mmproj"] = mmproj;
        v["model"] = native;
        if (!v.contains("gpu")) v["gpu"] = false;
        if (!v.contains("max_tokens")) v["max_tokens"] = 768;
        if (!v.contains("threads")) v["threads"] = 8;
        cfg["vision"] = v;
        set_flag(args, "--vision", true);
    } else {
        cfg.erase("vision");
        set_flag(args, "--vision", false);
    }
    cfg["args"] = args;
    cfg["args_written"] = args;

    if (!util::write_json_atomic(config_path, cfg, err)) return false;
    model["config"] = config_path;
    return true;
}

bool ConfigStore::select_model_locked(const std::string& id, std::string& err) {
    std::string want = id;
    if (want.empty()) { err = "未指定模型"; return false; }
    json models = models_locked();
    json chosen;
    for (const auto& m : models) {
        if (m.value("id", "") == want ||
            (!m.value("config", "").empty() && path_key(m.value("config", "")) == path_key(want)) ||
            (!m.value("native", "").empty() && path_key(m.value("native", "")) == path_key(want))) {
            chosen = m;
            break;
        }
    }
    if (chosen.is_null()) { err = "未找到模型: " + id; return false; }
    if (!chosen.value("installed", false)) {
        std::string reason = chosen.value("reason", "模型文件不完整");
        err = "模型不可用: " + reason;
        return false;
    }
    std::string path = chosen.value("config", "");
    if (path.empty() || !util::is_file(path)) {
        if (!materialize_model_config_locked(chosen, err)) return false;
        path = chosen.value("config", "");
    }
    if (!util::is_file(path)) { err = "模型配置不存在: " + path; return false; }
    std::string read_err;
    auto j = util::read_json(path, read_err);
    if (!j || !j->is_object()) { err = "模型配置不是有效 JSON: " + read_err; return false; }
    config_path_ = path;
    cfg_ = *j;
    if (!cfg_.contains("args") || !cfg_["args"].is_array()) cfg_["args"] = json::array();
    saved_user_args_ = cfg_.value("user_args", json::array());
    merge_user_args(cfg_);
    launcher_["config_path"] = config_path_;
    launcher_["strata_root"] = root_;
    launcher_["data_dir"] = data_dir_;
    std::string save_err;
    if (!save_launcher_locked(save_err)) { err = save_err; return false; }
    return true;
}

bool ConfigStore::select_model(const std::string& id, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    return select_model_locked(id, err);
}

bool ConfigStore::merge_user_args_into_config(std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (saved_user_args_.empty()) {
        cfg_.erase("user_args");
        return true;
    }
    if (!create_backup_locked(err)) return false;
    json args = cfg_.value("args", json::array());
    cfg_["args"] = args;
    cfg_["args_written"] = args;
    cfg_.erase("user_args");
    saved_user_args_ = json::array();
    if (!save_config_locked(err)) return false;
    return true;
}

bool ConfigStore::save_as(const std::string& name, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    std::string n = util::trim(name);
    if (n.size() > 5 && n.substr(n.size() - 5) == ".json")
        n = n.substr(0, n.size() - 5);
    if (n.empty()) { err = "配置名称不能为空"; return false; }
    if (n == "." || n == ".." || n.find("..") != std::string::npos) {
        err = "配置名称不能包含 ..";
        return false;
    }
    for (char c : n) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            err = "配置名称不能包含路径或非法字符";
            return false;
        }
    }
    if (n.rfind("strata-", 0) != 0) n = "strata-" + n;
    std::string path = root_ + "\\" + n + ".json";
    if (util::is_file(path)) { err = "配置已存在: " + n + ".json"; return false; }
    config_path_ = path;
    if (!save_config_locked(err)) return false;
    launcher_["config_path"] = config_path_;
    launcher_["strata_root"] = root_;
    if (!save_launcher_locked(err)) return false;
    return true;
}

bool ConfigStore::set_data_dir(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (!util::is_dir(path)) { err = "模型位置不存在: " + path; return false; }
    data_dir_ = canonical_dir(path);
    launcher_["data_dir"] = data_dir_;
    if (!save_launcher_locked(err)) return false;
    return true;
}

std::string ConfigStore::shared_path_locked() const {
    if (config_path_.empty()) return {};
    return parent_of(config_path_) + "\\" + stem_of(config_path_) + ".shared-settings.json";
}

std::string ConfigStore::detect_mtp_path_locked() const {
    std::vector<std::string> candidates;
    std::string appdata = util::get_env_utf8(L"APPDATA");
    if (!appdata.empty()) {
        std::string err;
        auto settings = util::read_json(appdata + "\\Strata\\settings.json", err);
        if (settings && settings->is_object()) {
            std::string data_dir = sval(*settings, "data_dir");
            if (!data_dir.empty()) candidates.push_back(data_dir + "\\mtp\\rt");
        }
    }
    if (!root_.empty()) {
        candidates.push_back(util::u8(util::path(root_).parent_path() / L"Strata-data" / L"mtp" / L"rt"));
        candidates.push_back(root_ + "\\Strata-data\\mtp\\rt");
        candidates.push_back(root_ + "\\..\\Strata-data\\mtp\\rt");
    }
    for (const auto& p : candidates) {
        if (util::is_file(p + "\\experts.bin")) return util::u8(fs::weakly_canonical(util::path(p)));
    }
    for (const auto& p : candidates) if (util::is_dir(p)) return util::u8(fs::weakly_canonical(util::path(p)));
    return candidates.empty() ? "" : candidates.front();
}

json ConfigStore::read_shared_locked() const {
    std::string p = shared_path_locked();
    if (p.empty() || !util::is_file(p)) return json::object();
    std::string err;
    auto j = util::read_json(p, err);
    if (!j || !j->is_object()) return json::object();
    return *j;
}

bool ConfigStore::save_shared_locked(const json& shared, std::string& err) {
    std::string p = shared_path_locked();
    if (p.empty()) return true;
    if (!shared.is_object() || shared.empty()) {
        std::error_code ec;
        fs::remove(util::path(p), ec);
        return true;
    }
    return util::write_json_atomic(p, shared, err);
}

std::string ConfigStore::backup_dir_locked() const {
    if (root_.empty() || config_path_.empty()) return {};
    return root_ + "\\config-backups\\" + stem_of(config_path_);
}

bool ConfigStore::create_backup_locked(std::string& err) {
    if (config_path_.empty() || !util::is_file(config_path_)) return true;
    std::string dir = backup_dir_locked();
    std::error_code ec;
    fs::create_directories(util::path(dir), ec);
    if (ec) { err = "无法创建备份目录: " + dir; return false; }
    std::string base = stem_of(config_path_) + "." + util::timestamp_compact();
    std::string target = dir + "\\" + base + ".bak";
    for (int i = 1; util::is_file(target) && i < 100; ++i) {
        target = dir + "\\" + base + "-" + std::to_string(i) + ".bak";
    }
    fs::copy_file(util::path(config_path_), util::path(target),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "备份失败: " + ec.message(); return false; }
    prune_backups_locked();
    return true;
}

void ConfigStore::prune_backups_locked() {
    std::string dir = backup_dir_locked();
    if (dir.empty()) return;
    int keep = std::clamp(ival(launcher_, "backup_count", 5), 1, 50);
    std::vector<fs::directory_entry> items;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(util::path(dir), ec)) {
        if (ec) break;
        if (e.is_regular_file() &&
            e.path().extension() == L".bak") items.push_back(e);
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        return fs::last_write_time(a.path(), e1) > fs::last_write_time(b.path(), e2);
    });
    for (size_t i = (size_t)keep; i < items.size(); ++i) {
        std::error_code de;
        fs::remove(items[i].path(), de);
    }
}

bool ConfigStore::save_config_locked(std::string& err) {
    if (config_path_.empty()) { err = "没有选中的配置文件"; return false; }
    if (!create_backup_locked(err)) return false;
    return util::write_json_atomic(config_path_, cfg_, err);
}

json ConfigStore::backups_locked() const {
    json out = json::array();
    std::string dir = backup_dir_locked();
    if (dir.empty()) return out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(util::path(dir), ec)) {
        if (ec) break;
        if (!e.is_regular_file() || e.path().extension() != L".bak") continue;
        std::string p = util::u8(e.path());
        std::string model;
        int context = 0;
        std::string kv;
        std::string read_err;
        auto j = util::read_json(p, read_err);
        if (j && j->is_object()) {
            model = sval(*j, "model_name");
            auto args = j->value("args", json::array());
            try { context = std::stoi(arg_value(args, "--max-context").value_or("0")); } catch (...) {}
            kv = arg_value(args, "--kv").value_or("");
        }
        out.push_back(json{{"name", util::u8(e.path().filename())},
                           {"path", p},
                           {"mtime", file_mtime_epoch(p)},
                           {"mtime_text", mtime_text(p)},
                           {"size", util::file_size_str(p)},
                           {"model_name", model},
                           {"context", context},
                           {"kv", kv}});
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return a.value("mtime", 0) > b.value("mtime", 0);
    });
    return out;
}

json ConfigStore::backups() const {
    std::lock_guard<std::mutex> lk(m_);
    return backups_locked();
}

bool ConfigStore::restore_backup(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (!util::is_file(path)) { err = "备份不存在: " + path; return false; }
    std::string dir = backup_dir_locked();
    std::string canonical_dir = util::u8(fs::weakly_canonical(util::path(dir)));
    std::string canonical_file = util::u8(fs::weakly_canonical(util::path(path)));
    if (canonical_dir.empty() ||
        canonical_file.rfind(canonical_dir + "\\", 0) != 0) {
        err = "只能恢复当前配置的备份";
        return false;
    }
    std::string read_err;
    auto j = util::read_json(path, read_err);
    if (!j || !j->is_object()) { err = "备份不是有效 JSON: " + read_err; return false; }
    if (!create_backup_locked(err)) return false;
    std::error_code ec;
    fs::copy_file(util::path(path), util::path(config_path_),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "恢复失败: " + ec.message(); return false; }
    cfg_ = *j;
    if (!cfg_.contains("args") || !cfg_["args"].is_array()) cfg_["args"] = json::array();
    saved_user_args_ = cfg_.value("user_args", json::array());
    merge_user_args(cfg_);
    return true;
}

bool ConfigStore::apply(const json& form, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (config_path_.empty()) { err = "没有选中的配置文件"; return false; }
    if (!form.is_object()) { err = "参数格式错误"; return false; }

    auto str = [&](const char* k, const std::string& def = "") {
        return form.contains(k) && form[k].is_string() ? form[k].get<std::string>() : def;
    };
    auto num = [&](const char* k, double def = 0) {
        if (!form.contains(k)) return def;
        try {
            if (form[k].is_number()) return form[k].get<double>();
            if (form[k].is_string()) return std::stod(form[k].get<std::string>());
        } catch (...) {}
        return def;
    };
    auto flag = [&](const char* k, bool def = false) {
        return form.contains(k) && form[k].is_boolean() ? form[k].get<bool>() : def;
    };

    json args = cfg_.value("args", json::array());
    if (!args.is_array()) args = json::array();

    if (form.contains("model_name")) cfg_["model_name"] = str("model_name");
    if (form.contains("aliases")) {
        auto a = text_list(str("aliases"));
        if (a.empty()) cfg_.erase("aliases"); else cfg_["aliases"] = a;
    }
    if (form.contains("exe") && !str("exe").empty()) cfg_["exe"] = str("exe");
    if (form.contains("tokenizer")) cfg_["tokenizer"] = str("tokenizer");
    if (form.contains("log")) cfg_["log"] = str("log");
    if (form.contains("data_dir")) {
        std::string d = str("data_dir");
        if (!d.empty()) {
            if (!util::is_dir(d)) { err = "模型位置不存在: " + d; return false; }
            data_dir_ = canonical_dir(d);
        }
    }
    if (form.contains("host") && !str("host").empty()) cfg_["host"] = str("host");
    if (form.contains("port")) {
        int port = (int)num("port", 8080);
        if (port < 1 || port > 65535) { err = "端口必须在 1-65535 之间"; return false; }
        cfg_["port"] = port;
    }

    if (form.contains("native")) {
        std::string v = str("native");
        if (v.empty()) remove_arg(args, "--native"); else set_arg(args, "--native", v);
    }
    if (form.contains("ple")) {
        std::string v = str("ple");
        if (v.empty()) remove_arg(args, "--ple-gguf"); else set_arg(args, "--ple-gguf", v);
    }
    if (form.contains("pack")) {
        std::string v = str("pack");
        if (v.empty()) remove_arg(args, "--pack"); else set_arg(args, "--pack", v);
    }
    if (form.contains("expert_profile")) {
        std::string v = str("expert_profile");
        if (v.empty()) remove_arg(args, "--expert-profile");
        else set_arg(args, "--expert-profile", v);
    }

    if (form.contains("context")) {
        int ctx = (int)num("context", 0);
        if (ctx <= 0) { err = "上下文长度必须大于 0"; return false; }
        set_arg(args, "--max-context", std::to_string(ctx));
    }
    if (form.contains("kv")) {
        std::string kv = str("kv", "int8");
        if (kv != "fp16" && kv != "int8" && kv != "q4_0" && kv != "k8v4") {
            err = "KV 类型只支持 fp16 / int8 / q4_0 / k8v4";
            return false;
        }
        set_arg(args, "--kv", kv);
    }

    if (form.contains("low_ram")) {
        set_flag(args, "--mmap-experts", false);
        set_flag(args, "--resident-experts", false);
        std::string v = str("low_ram");
        if (v == "mmap" || v == "budget") set_flag(args, "--mmap-experts", true);
        else if (v == "resident") set_flag(args, "--resident-experts", true);
    }

    if (form.contains("mtp")) {
        bool on = flag("mtp", false);
        if (!on) {
            remove_arg(args, "--mtp");
        } else {
            const bool native_mtp = has_native_mtp_locked();
            std::string v = util::trim(str("mtp_path"));
            std::string existing = util::trim(arg_value(args, "--mtp").value_or(""));
            bool numeric = !v.empty() && std::all_of(v.begin(), v.end(),
                [](unsigned char c) { return std::isdigit(c) != 0; });
            if (native_mtp) {
                if (!v.empty()) {
                    err = "该模型自带草稿，不能设置 MTP 路径；请使用投机窗口/MTP 草稿数";
                    return false;
                }
                std::string drafts = util::trim(str("spec"));
                if (drafts.empty()) {
                    drafts = (!existing.empty() && std::all_of(existing.begin(), existing.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) ? existing : "2";
                }
                if (!std::all_of(drafts.begin(), drafts.end(),
                                 [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    err = "该模型自带草稿，草稿数只能填写 1-7 的整数";
                    return false;
                }
                int n = 0;
                try { n = std::stoi(drafts); } catch (...) { n = 0; }
                if (n < 1 || n > 7) { err = "该模型自带草稿，草稿数必须是 1-7 的整数"; return false; }
                set_arg(args, "--mtp", std::to_string(n));
            } else {
                if (v.empty()) v = existing;
                if (v.empty()) v = detect_mtp_path_locked();
                if (numeric) {
                    err = "当前模型需要 MTP runtime 路径；自带草稿的模型才填写草稿数";
                    return false;
                }
                if (v.empty()) { err = "未找到 MTP runtime，请填写路径"; return false; }
                if (!util::is_file(v + "\\experts.bin")) {
                    err = "MTP runtime 缺少 experts.bin: " + v;
                    return false;
                }
                set_arg(args, "--mtp", v);
            }
        }
    }

    if (form.contains("drafting")) {
        std::string mode = util::trim(str("drafting"));
        if (mode.empty()) mode = "auto";
        if (mode != "auto" && mode != "mtp" && mode != "dflash2" && mode != "off") {
            err = "草稿类型必须是 auto、mtp、dflash2 或 off";
            return false;
        }
        if (form.contains("mtp") && !flag("mtp", false)) {
            mode = "off";
        }
        if (mode == "off") {
            remove_arg(args, "--mtp");
            cfg_["mtp"] = false;
        } else {
            cfg_.erase("mtp");
        }
        cfg_["drafting"] = mode;
    }

    auto set_optional = [&](const char* key, const char* flag_name) {
        if (!form.contains(key)) return;
        std::string v = str(key);
        if (v.empty()) remove_arg(args, flag_name); else set_arg(args, flag_name, v);
    };
    if (has_native_mtp_locked()) {
        remove_arg(args, "--spec");
        remove_arg(args, "--spec-min-p");
    } else {
        set_optional("spec", "--spec");
        set_optional("spec_min_p", "--spec-min-p");
    }
    set_optional("prefill", "--prefill");
    set_optional("expert_cache", "--expert-cache");
    set_optional("kv_resident", "--kv-resident");
    if (form.contains("vram_reserve")) {
        int v = (int)num("vram_reserve", 0);
        if (v <= 0) remove_arg(args, "--vram-reserve-mib");
        else set_arg(args, "--vram-reserve-mib", std::to_string(v));
    }
    if (form.contains("vram_elastic")) {
        set_bool_or_erase(cfg_, "vram_elastic", flag("vram_elastic", false));
    }
    auto optional_int = [&](const char* key, long long min, long long max) -> bool {
        if (!form.contains(key)) return true;
        std::string s = util::trim(str(key));
        if (s.empty()) { cfg_.erase(key); return true; }
        size_t pos = 0;
        long long n = 0;
        try { n = std::stoll(s, &pos); } catch (...) { pos = 0; }
        if (pos != s.size() || n < min || n > max) {
            err = std::string(key) + " 超出范围";
            return false;
        }
        cfg_[key] = n;
        return true;
    };
    if (!optional_int("vram_segment_mib", 128, 16384)) return false;
    if (form.contains("expert_profile_save")) {
        std::string v = util::trim(str("expert_profile_save"));
        if (v.empty()) cfg_.erase("expert_profile_save"); else cfg_["expert_profile_save"] = v;
    }
    if (!optional_int("expert_profile_save_every", 0, 10080)) return false;
    if (!optional_int("parallel", 1, 8)) return false;
    if (!optional_int("max_queue", 0, 4096)) return false;
    if (form.contains("queue_timeout")) {
        std::string s = util::trim(str("queue_timeout"));
        if (s.empty()) {
            cfg_.erase("queue_timeout");
        } else {
            try {
                double v = std::stod(s);
                if (v < 0.1 || v > 600) { err = "queue_timeout 必须在 0.1-600 秒之间"; return false; }
                if (!cfg_.contains("max_queue")) {
                    err = "设置 queue_timeout 前请先设置 max_queue";
                    return false;
                }
                cfg_["queue_timeout"] = v;
            } catch (...) {
                err = "queue_timeout 不是数字";
                return false;
            }
        }
    }
    if (!optional_int("engine_ready_s", 1, 86400)) return false;
    if (form.contains("resident_budget_gib")) {
        std::string s = util::trim(str("resident_budget_gib"));
        std::string low = form.contains("low_ram") ? util::trim(str("low_ram")) : "";
        if (s.empty() || s == "0") {
            if (low == "budget") {
                err = "预算缓存模式需要填写 RAM 专家预算"; return false;
            }
            remove_arg(args, "--resident-budget-gib");
        } else {
            size_t pos = 0; long long n = 0;
            try { n = std::stoll(s, &pos); } catch (...) { pos = 0; }
            if (pos != s.size() || n < 1 || n > 1024) {
                err = "RAM 专家预算必须是 1-1024 GiB"; return false;
            }
            if (low == "resident") {
                err = "resident 模式与 RAM 专家预算互斥；请选择预算缓存"; return false;
            }
            set_arg(args, "--resident-budget-gib", std::to_string(n));
            set_flag(args, "--mmap-experts", true);
        }
    }
    if (form.contains("pool_workers")) {
        std::string s = util::trim(str("pool_workers"));
        if (s.empty()) remove_arg(args, "--pool-workers");
        else {
            size_t pos = 0; long long n = 0;
            try { n = std::stoll(s, &pos); } catch (...) { pos = 0; }
            if (pos != s.size() || n < 1 || n > 256) {
                err = "CPU 专家线程必须是 1-256"; return false;
            }
            set_arg(args, "--pool-workers", std::to_string(n));
        }
    }
    if (form.contains("ple_io")) {
        std::string v = util::trim(str("ple_io"));
        if (v.empty() || v == "auto") remove_arg(args, "--ple-io");
        else if (v == "direct" || v == "ram" || v == "mmap") set_arg(args, "--ple-io", v);
        else { err = "PLE I/O 必须是 auto、direct、ram 或 mmap"; return false; }
    }

    auto conv_int = [&](const char* arg, const char* key, long long lo, long long hi) -> bool {
        if (!form.contains(key)) return true;
        std::string v = util::trim(str(key));
        if (v.empty()) { remove_arg(args, arg); return true; }
        size_t pos = 0; long long n = 0;
        try { n = std::stoll(v, &pos); } catch (...) { pos = 0; }
        if (pos != v.size() || n < lo || n > hi) {
            err = std::string(key) + " 超出范围"; return false;
        }
        set_arg(args, arg, std::to_string(n));
        return true;
    };
    if (!conv_int("--conversation-cache-mib", "conversation_cache_mib", 0, 32768)) return false;
    if (!conv_int("--conversation-cache-slots", "conversation_cache_slots", 1, 32)) return false;
    if (!conv_int("--conversation-cache-min-free-mib", "conversation_cache_min_free_mib", 0, 65536)) return false;
    if (!conv_int("--conversation-cache-min-tokens", "conversation_cache_min_tokens", 0, 1000000)) return false;
    if (!conv_int("--conversation-cache-park-gain", "conversation_cache_park_gain", 0, 1000000)) return false;
    if (form.contains("vision")) {
        bool on = flag("vision", false);
        if (on) {
            std::string exe = str("vision_exe");
            std::string mmproj = str("vision_mmproj");
            if (exe.empty() && cfg_.contains("vision") && cfg_["vision"].is_object())
                exe = sval(cfg_["vision"], "exe");
            if (mmproj.empty() && cfg_.contains("vision") && cfg_["vision"].is_object())
                mmproj = sval(cfg_["vision"], "mmproj");
            if (exe.empty()) exe = root_ + "\\engine\\strata-vision.exe";
            if (mmproj.empty()) {
                std::string native = arg_value(args, "--native").value_or("");
                std::string dir = parent_of(native);
                std::error_code ec;
                if (!dir.empty()) {
                    for (auto& e : fs::directory_iterator(util::path(dir), ec)) {
                        if (ec) break;
                        std::string n = util::u8(e.path().filename());
                        if (n.rfind("mmproj", 0) == 0 && e.path().extension() == L".gguf") {
                            mmproj = util::u8(e.path());
                            break;
                        }
                    }
                }
            }
            bool want_gpu = flag("vision_gpu", false);
            std::string gpu_exe = find_vision_exe(root_, true);
            std::string cpu_exe = find_vision_exe(root_, false);
            if (want_gpu && !gpu_exe.empty() && (exe.empty() || exe == cpu_exe))
                exe = gpu_exe;
            if (!want_gpu && !cpu_exe.empty() && (exe.empty() || exe == gpu_exe))
                exe = cpu_exe;
            if (!util::is_file(exe)) { err = "视觉编码器不存在: " + exe; return false; }
            if (!util::is_file(mmproj)) { err = "视觉 mmproj 不存在: " + mmproj; return false; }
            json v = cfg_.contains("vision") && cfg_["vision"].is_object()
                         ? cfg_["vision"] : json::object();
            v["exe"] = exe;
            v["mmproj"] = mmproj;
            v["model"] = arg_value(args, "--native").value_or(sval(v, "model"));
            v["gpu"] = want_gpu;
            int max_tokens = form.contains("vision_max_tokens")
                ? (int)num("vision_max_tokens", ival(v, "max_tokens", 768))
                : ival(v, "max_tokens", 768);
            int threads = form.contains("vision_threads")
                ? (int)num("vision_threads", ival(v, "threads", 8))
                : ival(v, "threads", 8);
            if (max_tokens < 64 || max_tokens > 4096) {
                err = "视觉最大 tokens 必须在 64-4096 之间"; return false;
            }
            if (threads < 1 || threads > 128) {
                err = "视觉线程数必须在 1-128 之间"; return false;
            }
            v["max_tokens"] = max_tokens;
            v["threads"] = threads;
            v["elastic"] = flag("vision_elastic", bval(v, "elastic", false));
            int elastic_idle = form.contains("vision_elastic_idle_s")
                ? (int)num("vision_elastic_idle_s", ival(v, "elastic_idle_s", 120))
                : ival(v, "elastic_idle_s", 120);
            int elastic_mib = form.contains("vision_elastic_mib")
                ? (int)num("vision_elastic_mib", ival(v, "elastic_mib", 2048))
                : ival(v, "elastic_mib", 2048);
            int min_tokens = form.contains("vision_min_tokens")
                ? (int)num("vision_min_tokens", ival(v, "min_tokens", 0))
                : ival(v, "min_tokens", 0);
            if (elastic_idle < 0 || elastic_idle > 86400) {
                err = "视觉空闲卸载秒数必须在 0-86400 之间"; return false;
            }
            if (elastic_mib < 128 || elastic_mib > 16384) {
                err = "视觉弹性显存必须在 128-16384 MiB 之间"; return false;
            }
            if (min_tokens < 0 || min_tokens > 4096) {
                err = "视觉最小 tokens 必须在 0-4096 之间"; return false;
            }
            v["elastic_idle_s"] = elastic_idle;
            v["elastic_mib"] = elastic_mib;
            v["min_tokens"] = min_tokens;
            cfg_["vision"] = v;
            set_flag(args, "--vision", true);
        } else {
            cfg_.erase("vision");
            set_flag(args, "--vision", false);
        }
    }

    if (form.contains("idle_unload_min")) {
        double minutes = std::max(0.0, num("idle_unload_min", 60));
        int seconds = (int)std::llround(minutes * 60.0);
        cfg_["idle_unload_s"] = seconds;
    }

    if (form.contains("gpu")) {
        std::string g = str("gpu");
        bool roles_active = (cfg_.contains("gpu_roles") &&
                             ((cfg_["gpu_roles"].is_object() && !cfg_["gpu_roles"].empty()) ||
                              (cfg_["gpu_roles"].is_array() && !cfg_["gpu_roles"].empty())));
        if (roles_active && g == gpu_text(cfg_)) {
            // The new Strata app/Terminal owns Main/Helper/Off roles; a plain save must not
            // turn them into a hand-edited legacy "gpu" setting.
        } else if (roles_active) {
            cfg_.erase("gpu_roles");
            cfg_.erase("gpu_order");
            if (g.empty()) { cfg_.erase("gpu"); cfg_.erase("gpus"); }
            else cfg_["gpu"] = parse_gpu(g);
        } else if (g.empty()) {
            cfg_.erase("gpu"); cfg_.erase("gpus");
        } else {
            cfg_["gpu"] = parse_gpu(g);
        }
    }

    if (form.contains("api_key_clear") && flag("api_key_clear", false)) {
        cfg_.erase("api_key");
    } else if (form.contains("api_key")) {
        std::string key = str("api_key");
        if (!key.empty()) cfg_["api_key"] = key;
    }

    if (form.contains("cors_origins")) {
        auto a = text_list(str("cors_origins"));
        if (a.empty()) cfg_.erase("cors_origins"); else cfg_["cors_origins"] = a;
    }
    if (form.contains("trusted_origins")) {
        auto a = text_list(str("trusted_origins"));
        if (a.empty()) cfg_.erase("trusted_origins"); else cfg_["trusted_origins"] = a;
    }
    if (form.contains("allowed_hosts")) {
        auto a = text_list(str("allowed_hosts"));
        if (a.empty()) cfg_.erase("allowed_hosts"); else cfg_["allowed_hosts"] = a;
    }

    if (form.contains("open_browser")) set_bool_or_erase(cfg_, "open_browser", flag("open_browser", false));
    if (form.contains("prevent_sleep")) set_bool_or_erase(cfg_, "prevent_sleep", flag("prevent_sleep", false));
    if (form.contains("api_monitor")) set_bool_or_erase(cfg_, "api_monitor", flag("api_monitor", false));
    if (form.contains("start_mode")) {
        bool lazy = str("start_mode") == "lazy";
        set_bool_or_erase(cfg_, "lazy_load", lazy);
    }

    if (form.contains("reasoning_effort")) {
        std::string v = str("reasoning_effort");
        if (v.empty() || v == "default") {
            cfg_.erase("reasoning_effort");
        } else if (v == "none" || v == "low" || v == "medium" || v == "high") {
            cfg_["reasoning_effort"] = v;
        } else {
            err = "默认思考强度只支持 none / low / medium / high";
            return false;
        }
    }

    if (form.contains("reasoning_budget_tokens")) {
        std::string text;
        if (form["reasoning_budget_tokens"].is_string())
            text = util::trim(form["reasoning_budget_tokens"].get<std::string>());
        else if (form["reasoning_budget_tokens"].is_number())
            text = std::to_string((long long)form["reasoning_budget_tokens"].get<double>());
        if (text.empty()) {
            cfg_.erase("reasoning_budget_tokens");
        } else {
            size_t pos = 0;
            long long n = 0;
            try { n = std::stoll(text, &pos); } catch (...) { pos = 0; }
            if (pos != text.size() || n < 0 || n > 1000000) {
                err = "Reasoning Budget 必须是 0-1000000 的整数（0 表示不限制）";
                return false;
            }
            cfg_["reasoning_budget_tokens"] = n;
        }
    }

    if (form.contains("answer_room")) {
        bool on = flag("answer_room", true);
        if (on) cfg_.erase("answer_room"); else cfg_["answer_room"] = false;
    }
    if (form.contains("reasoning_close_retry")) {
        set_bool_or_erase(cfg_, "reasoning_close_retry", flag("reasoning_close_retry", false));
    }
    if (form.contains("reasoning_loop_recovery")) {
        std::string v = util::trim(str("reasoning_loop_recovery"));
        if (v.empty() || v == "off") cfg_.erase("reasoning_loop_recovery");
        else if (v == "stop") cfg_["reasoning_loop_recovery"] = "stop";
        else if (v == "recover" || v == "true") cfg_["reasoning_loop_recovery"] = true;
        else { err = "reasoning_loop_recovery 必须是 off、stop 或 recover"; return false; }
    }
    if (form.contains("repeat_stop_tokens")) {
        std::string v = util::trim(str("repeat_stop_tokens"));
        if (v.empty() || v == "256") cfg_.erase("repeat_stop_tokens");
        else {
            size_t pos = 0; long long n = 0;
            try { n = std::stoll(v, &pos); } catch (...) { pos = 0; }
            if (pos != v.size() || n < 0 || n > 1000000) {
                err = "repeat_stop_tokens 必须是 0-1000000 的整数"; return false;
            }
            cfg_["repeat_stop_tokens"] = n;
        }
    }
    if (form.contains("tool_call_recovery")) {
        bool on = flag("tool_call_recovery", true);
        if (on) cfg_.erase("tool_call_recovery"); else cfg_["tool_call_recovery"] = false;
    }
    if (form.contains("effort_position")) {
        std::string v = util::trim(str("effort_position"));
        if (v.empty() || v == "start") cfg_.erase("effort_position");
        else if (v == "end") cfg_["effort_position"] = "end";
        else { err = "effort_position 必须是 start 或 end"; return false; }
    }
    if (form.contains("anthropic_thinking")) {
        std::string v = util::trim(str("anthropic_thinking"));
        if (v.empty() || v == "model") cfg_.erase("anthropic_thinking");
        else if (v == "on_request") cfg_["anthropic_thinking"] = "on_request";
        else { err = "anthropic_thinking 必须是 model 或 on_request"; return false; }
    }
    if (form.contains("codex_thread_titles")) {
        set_bool_or_erase(cfg_, "codex_thread_titles", flag("codex_thread_titles", false));
    }
    if (form.contains("codex_compaction_cache")) {
        set_bool_or_erase(cfg_, "codex_compaction_cache", flag("codex_compaction_cache", false));
    }

    {
        json sampling = cfg_.value("sampling", json::object());
        if (!sampling.is_object()) sampling = json::object();
        auto sampling_text = [&](const char* key) {
            if (!form.contains(key)) return std::optional<std::string>{};
            if (form[key].is_null()) return std::optional<std::string>{std::string()};
            if (form[key].is_string()) return std::optional<std::string>{form[key].get<std::string>()};
            if (form[key].is_number()) return std::optional<std::string>{std::to_string(form[key].get<double>())};
            return std::optional<std::string>{std::string()};
        };
        auto apply_number = [&](const char* key, double min, double max, bool integer) -> bool {
            auto v = sampling_text(key);
            if (!v) return true;
            std::string s = util::trim(*v);
            if (s.empty()) { sampling.erase(key); return true; }
            try {
                double d = std::stod(s);
                if (d < min || d > max || (integer && std::floor(d) != d)) {
                    err = std::string("采样参数 ") + key + " 超出范围";
                    return false;
                }
                sampling[key] = integer ? json((long long)d) : json(d);
                return true;
            } catch (...) {
                err = std::string("采样参数 ") + key + " 不是数字";
                return false;
            }
        };
        if (!apply_number("temperature", 0, 2, false)) return false;
        if (!apply_number("top_p", 0.000001, 1, false)) return false;
        if (!apply_number("top_k", 1, 64, true)) return false;
        if (!apply_number("min_p", 0, 1, false)) return false;
        if (!apply_number("presence_penalty", 0, 4, false)) return false;
        if (!apply_number("frequency_penalty", 0, 4, false)) return false;
        if (!apply_number("repetition_penalty", 0.01, 4, false)) return false;
        if (!apply_number("penalty_last_n", 0, 1000000, true)) return false;
        if (!apply_number("seed", 1, 9.0e18, true)) return false;
        if (sampling.empty()) cfg_.erase("sampling");
        else cfg_["sampling"] = sampling;
    }

    if (form.contains("fit_max_tokens")) {
        set_bool_or_erase(cfg_, "fit_max_tokens", flag("fit_max_tokens", false));
    }

    if (form.contains("max_tokens")) {
        std::string text;
        if (form["max_tokens"].is_string()) text = util::trim(form["max_tokens"].get<std::string>());
        else if (form["max_tokens"].is_number()) text = std::to_string((long long)form["max_tokens"].get<double>());
        json shared = read_shared_locked();
        if (text.empty()) {
            shared.erase("max_tokens");
        } else {
            try {
                long long n = std::stoll(text);
                if (n <= 0 || n > 1000000) { err = "Max Tokens 必须是 1-1000000 的整数"; return false; }
                shared["max_tokens"] = (int)n;
            } catch (...) {
                err = "Max Tokens 必须是正整数";
                return false;
            }
        }
        if (!save_shared_locked(shared, err)) return false;
    }

    if (form.contains("auto_restart")) launcher_["auto_restart"] = flag("auto_restart", false);
    if (form.contains("lazy_on_restart")) launcher_["lazy_on_restart"] = flag("lazy_on_restart", true);
    if (form.contains("backup_count")) {
        launcher_["backup_count"] = std::clamp((int)num("backup_count", 5), 1, 50);
    }
    launcher_["config_path"] = config_path_;
    launcher_["strata_root"] = root_;
    launcher_["data_dir"] = data_dir_;

    cfg_["args"] = args;
    json kept_user = update_user_args(saved_user_args_, args);
    if (kept_user.is_array() && !kept_user.empty()) cfg_["user_args"] = kept_user;
    else cfg_.erase("user_args");
    cfg_["args_written"] = args;
    if (!save_config_locked(err)) return false;
    std::string launcher_err;
    if (!save_launcher_locked(launcher_err)) { err = launcher_err; return false; }
    return true;
}

bool ConfigStore::auto_restart() const {
    std::lock_guard<std::mutex> lk(m_);
    return bval(launcher_, "auto_restart", true);
}

bool ConfigStore::lazy_on_restart() const {
    std::lock_guard<std::mutex> lk(m_);
    return bval(launcher_, "lazy_on_restart", true);
}

std::wstring ConfigStore::build_command_locked(bool force_lazy) const {
    std::string python = root_ + "\\.venv\\Scripts\\python.exe";
    if (!util::is_file(python)) python = "python.exe";
    std::string server = root_ + "\\serve\\server.py";
    int port = model_port_locked();
    std::string host = sval(cfg_, "host", "127.0.0.1");
    if (host.empty()) host = "127.0.0.1";

    std::wstring cmd = util::quote_arg(util::widen(python)) + L" -u " +
                       util::quote_arg(util::widen(server)) +
                       L" --engine strata --config " + util::quote_arg(util::widen(config_path_)) +
                       L" --port " + std::to_wstring(port) +
                       L" --host " + util::quote_arg(util::widen(host));
    bool lazy = force_lazy || bval(cfg_, "lazy_load", false);
    if (lazy) cmd += L" --lazy";
    if (bval(cfg_, "open_browser", false)) cmd += L" --open";
    return cmd;
}

std::wstring ConfigStore::build_command(bool force_lazy) const {
    std::lock_guard<std::mutex> lk(m_);
    return build_command_locked(force_lazy);
}

bool ConfigStore::start_preflight(std::string& err) const {
    std::lock_guard<std::mutex> lk(m_);
    if (config_path_.empty()) { err = "没有选中的配置文件"; return false; }
    if (!util::is_file(config_path_)) { err = "配置文件不存在: " + config_path_; return false; }
    std::string server = root_ + "\\serve\\server.py";
    if (!util::is_file(server)) { err = "找不到 serve/server.py: " + server; return false; }
    std::string python = root_ + "\\.venv\\Scripts\\python.exe";
    if (!util::is_file(python)) { err = "找不到 Strata Python: " + python; return false; }
    std::string exe = sval(cfg_, "exe");
    if (!util::is_file(exe)) { err = "引擎 exe 不存在: " + exe; return false; }
    auto args = cfg_.value("args", json::array());
    std::string pack = arg_value(args, "--pack").value_or("");
    if (!pack.empty() && !util::is_dir(pack)) { err = "Pack 目录不存在: " + pack; return false; }
    std::string native = arg_value(args, "--native").value_or("");
    if (!native.empty() && !util::is_file(native)) { err = "模型首分片不存在: " + native; return false; }
    std::string tokenizer = sval(cfg_, "tokenizer");
    if (!tokenizer.empty() && !util::is_file(tokenizer + "\\vocab.json")) {
        err = "找不到 tokenizer/vocab.json: " + tokenizer; return false;
    }
    if (vision_enabled_locked()) {
        std::string vexe = cfg_.contains("vision") ? sval(cfg_["vision"], "exe") : "";
        std::string vmm = cfg_.contains("vision") ? sval(cfg_["vision"], "mmproj") : "";
        if (!util::is_file(vexe)) { err = "视觉编码器不存在: " + vexe; return false; }
        if (!util::is_file(vmm)) { err = "视觉 mmproj 不存在: " + vmm; return false; }
    }
    int port = model_port_locked();
    if (port < 1 || port > 65535) { err = "端口无效"; return false; }
    if (!util::is_loopback_host(sval(cfg_, "host", "127.0.0.1")) && api_key_locked().empty()) {
        err = "监听非本机地址时必须设置 API Key"; return false;
    }
    return true;
}

json ConfigStore::command_preview() const {
    std::lock_guard<std::mutex> lk(m_);
    std::wstring cmd = build_command_locked(false);
    return json{{"server", util::narrow(cmd)},
                {"config", config_path_},
                {"engine_args", cfg_.value("args", json::array())},
                {"log", log_path_locked()}};
}

std::string ConfigStore::llama_swap_yaml() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string python = root_ + "\\.venv\\Scripts\\python.exe";
    std::string server = root_ + "\\serve\\server.py";
    int port = model_port_locked();
    int ttl = (int)std::llround(dval(cfg_, "idle_unload_s", 3600.0));
    if (ttl <= 0) ttl = 3600;
    std::string model_id = sval(cfg_, "model_name");
    if (model_id.empty()) {
        model_id = filename_of(config_path_);
        if (model_id.rfind("strata-", 0) == 0) model_id = model_id.substr(7);
        if (model_id.size() > 5 && model_id.substr(model_id.size() - 5) == ".json")
            model_id = model_id.substr(0, model_id.size() - 5);
    }
    std::string cmd = util::narrow(util::quote_arg(util::widen(python))) + " -u " +
                      util::narrow(util::quote_arg(util::widen(server))) +
                      " --engine strata --config " +
                      util::narrow(util::quote_arg(util::widen(config_path_))) +
                      " --port " + std::to_string(port) +
                      " --host 127.0.0.1 --lazy";
    std::ostringstream y;
    y << "# Strata Console v0.1.4 export\n"
      << "# 当前模型: " << model_id << "\n"
      << "# 默认使用当前 Strata 端口 " << port << "。若与其它 llama-swap 模型冲突，\n"
      << "# 请把 cmd 和 proxy 中的 " << port << " 都改为 ${PORT}。\n"
      << "# 客户端仍需要携带 Strata 配置中的 API Key。\n"
      << "healthCheckTimeout: 300\n"
      << "startPort: 12400\n"
      << "includeAliasesInList: true\n\n"
      << "models:\n"
      << "  " << yaml_quote(model_id) << ":\n"
      << "    cmd: " << yaml_quote(cmd) << "\n"
      << "    proxy: http://127.0.0.1:" << port << "\n"
      << "    checkEndpoint: /health\n"
      << "    ttl: " << ttl << "\n";
    std::vector<std::string> aliases;
    auto a = cfg_.value("aliases", json());
    if (a.is_array()) {
        for (const auto& v : a) if (v.is_string() && !v.get<std::string>().empty())
            aliases.push_back(v.get<std::string>());
    } else if (a.is_string()) {
        std::string s = a.get<std::string>();
        size_t start = 0;
        while (start <= s.size()) {
            size_t comma = s.find(',', start);
            std::string item = util::trim(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!item.empty()) aliases.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (!aliases.empty()) {
        y << "    aliases:\n";
        for (const auto& alias : aliases) y << "      - " << yaml_quote(alias) << "\n";
    }
    return y.str();
}