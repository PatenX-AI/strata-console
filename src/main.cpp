#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>

#include "config_store.h"
#include "process.h"
#include "util.h"

#include <commdlg.h>
#include <shlobj.h>

namespace {

constexpr const char* kVersion = "0.1.3";

struct App {
    std::string app_dir;
    std::string ui_dir;
    int ui_port = 8090;
    std::string csrf;
    std::string init_error;
    ConfigStore config;
    StatusCache status;
    ProcessManager process;
    httplib::Server* server = nullptr;

    App(std::string dir)
        : app_dir(std::move(dir)),
          ui_dir(app_dir + "\\ui"),
          config(app_dir),
          process(config, status) {
        status.set_history_path(app_dir + "\\strata-console-history.json");
    }
};

void send_json(httplib::Response& res, int code, const json& j) {
    res.status = code;
    res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace),
                    "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}

void send_error(httplib::Response& res, int code, const std::string& message) {
    send_json(res, code, json{{"ok", false}, {"error", message}});
}

bool host_ok(const httplib::Request& req) {
    std::string host = req.get_header_value("Host");
    if (host.empty()) return true;  // HTTP/1.0 or a local proxy that drops it
    if (host.rfind("127.0.0.1", 0) == 0 || host.rfind("localhost", 0) == 0 ||
        host.rfind("[::1]", 0) == 0) return true;
    return false;
}

bool origin_ok(const httplib::Request& req) {
    std::string origin = req.get_header_value("Origin");
    if (origin.empty()) return true;
    if (origin.rfind("http://127.0.0.1", 0) == 0 ||
        origin.rfind("http://localhost", 0) == 0 ||
        origin.rfind("http://[::1]", 0) == 0) return true;
    return false;
}

bool validate_post(const httplib::Request& req, httplib::Response& res) {
    if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return false; }
    if (!origin_ok(req)) { send_error(res, 403, "Origin 不被允许"); return false; }
    std::string ctype = req.get_header_value("Content-Type");
    if (ctype.rfind("application/json", 0) != 0) {
        send_error(res, 415, "Content-Type 必须是 application/json");
        return false;
    }
    return true;
}

bool check_csrf(const httplib::Request& req, httplib::Response& res, const std::string& token) {
    std::string got = req.get_header_value("X-CSRF-Token");
    if (got.empty() || got != token) { send_error(res, 403, "CSRF token 无效"); return false; }
    return true;
}

json parse_body(const httplib::Request& req, httplib::Response& res, bool& ok) {
    ok = false;
    try {
        json j = req.body.empty() ? json::object() : json::parse(req.body);
        if (!j.is_object()) { send_error(res, 400, "请求体必须是 JSON 对象"); return {}; }
        ok = true;
        return j;
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("JSON 解析失败: ") + e.what());
        return {};
    }
}

json config_payload(App& app) {
    return json{{"configs", app.config.configs()},
                {"models", app.config.models()},
                {"data_dir", app.config.data_dir()},
                {"active", app.config.config_path()},
                {"view", app.config.view()},
                {"backups", app.config.backups()},
                {"launcher", app.config.launcher_public()}};
}

json state_payload(App& app) {
    json state = json::object();
    state["app"] = json{{"version", kVersion}, {"ui_port", app.ui_port},
                        {"init_error", app.init_error}};
    state["config"] = app.config.view();
    state["configs"] = app.config.configs();
    state["process"] = app.process.status();
    state["status"] = app.status.state();
    state["launcher"] = app.config.launcher_public();
    state["now"] = util::now_iso();
    return state;
}

std::string python_exe(App& app) {
    std::string p = app.config.root() + "\\.venv\\Scripts\\python.exe";
    if (util::is_file(p)) return p;
    return "python.exe";
}

json run_auto_config(App& app) {
    std::string helper = app.app_dir + "\\auto_config_helper.py";
    if (!util::is_file(helper)) {
        return json{{"ok", false}, {"error", "auto_config_helper.py 不存在"}};
    }
    std::wstring cmd = util::quote_arg(util::widen(python_exe(app))) + L" " +
                       util::quote_arg(util::widen(helper)) +
                       L" --root " + util::quote_arg(util::widen(app.config.root()));
    auto r = util::run_capture(cmd, util::widen(app.config.root()), 30000);
    if (r.timed_out) return json{{"ok", false}, {"error", "自动配置检测超时"}};
    std::string out = util::trim(r.out);
    auto pos = out.find('{');
    auto last = out.rfind('}');
    if (pos == std::string::npos || last == std::string::npos || last < pos) {
        std::string msg = "自动配置没有返回 JSON";
        if (!r.err.empty()) msg += ": " + util::trim(r.err);
        return json{{"ok", false}, {"error", msg}};
    }
    try {
        return json::parse(out.substr(pos, last - pos + 1));
    } catch (const std::exception& e) {
        return json{{"ok", false}, {"error", std::string("自动配置 JSON 解析失败: ") + e.what()}};
    }
}

void add_security_headers(const httplib::Request& req, httplib::Response& res) {
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Referrer-Policy", "no-referrer");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
    res.set_header("Content-Security-Policy",
                   "default-src 'self'; base-uri 'none'; form-action 'self'; "
                   "script-src 'self'; style-src 'self' 'unsafe-inline'; "
                   "img-src 'self' data:; font-src 'self'; connect-src 'self'");
    if (req.path.rfind("/api/", 0) == 0) res.set_header("Cache-Control", "no-store");
    else if (req.path == "/" || req.path == "/index.html") res.set_header("Cache-Control", "no-store");
    else {
        auto ends_with = [](const std::string& s, const char* suffix) {
            size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        };
        if (ends_with(req.path, ".js") || ends_with(req.path, ".css") ||
            ends_with(req.path, ".woff2")) res.set_header("Cache-Control", "no-cache");
    }
}

bool quit_via_http(int port, const std::string& control_token, std::string& err) {
    try {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 500000);
        cli.set_read_timeout(0, 1500000);
        httplib::Headers h{{"X-Control-Token", control_token}};
        auto res = cli.Post("/api/quit", h, "{\"stop_model\":true}", "application/json");
        if (!res) { err = httplib::to_string(res.error()); return false; }
        if (res->status < 200 || res->status >= 300) {
            err = "HTTP " + std::to_string(res->status);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool daemon = false;
    bool open_browser = false;
    bool stop_only = false;
    int requested_port = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--daemon") daemon = true;
        else if (a == "--open") open_browser = true;
        else if (a == "--stop") stop_only = true;
        else if (a == "--port" && i + 1 < argc) requested_port = std::atoi(argv[++i]);
    }

    WSAStartup(MAKEWORD(2, 2), nullptr);

    const std::string app_dir = util::exe_dir();
    App app(app_dir);
    std::string init_err;
    app.config.init(init_err);
    app.init_error = init_err;
    app.ui_port = requested_port > 0 ? requested_port : app.config.ui_port();
    app.csrf = util::random_hex(24);

    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\StrataConsole.Singleton");
    bool already = singleton && GetLastError() == ERROR_ALREADY_EXISTS;
    if (already) {
        if (stop_only) {
            std::string err;
            if (!quit_via_http(app.ui_port, app.config.control_token(), err)) {
                util::open_url("http://127.0.0.1:" + std::to_string(app.ui_port) + "/");
            }
            if (singleton) CloseHandle(singleton);
            WSACleanup();
            return 0;
        }
        if (!daemon || open_browser)
            util::open_url("http://127.0.0.1:" + std::to_string(app.ui_port) + "/");
        if (singleton) CloseHandle(singleton);
        WSACleanup();
        return 0;
    }

    if (stop_only) {
        std::string err;
        quit_via_http(app.ui_port, app.config.control_token(), err);
        if (singleton) CloseHandle(singleton);
        WSACleanup();
        return 0;
    }

    if (!util::is_file(app.ui_dir + "\\index.html")) {
        MessageBoxW(nullptr, util::widen("找不到 UI 目录: " + app.ui_dir).c_str(),
                    L"Strata Console", MB_ICONERROR);
        if (singleton) CloseHandle(singleton);
        WSACleanup();
        return 1;
    }

    app.status.start();
    app.status.set_target(app.config.model_port(), app.config.api_key(), "");
    app.process.start_monitor();

    httplib::Server svr;
    app.server = &svr;
    svr.set_mount_point("/", app.ui_dir);
    svr.set_read_timeout(10, 0);
    svr.set_write_timeout(10, 0);
    svr.set_payload_max_length(1024 * 1024);
    svr.set_post_routing_handler(add_security_headers);
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try { if (ep) std::rethrow_exception(ep); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...) {}
        send_error(res, 500, msg);
    });

    svr.Get("/api/health", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        send_json(res, 200, json{{"ok", true}, {"app", "strata-console"},
                                 {"version", kVersion}, {"ui_port", app.ui_port},
                                 {"pid", (int)GetCurrentProcessId()}});
    });

    svr.Get("/api/session", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        send_json(res, 200, json{{"token", app.csrf}, {"version", kVersion}});
    });

    svr.Get("/api/state", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        send_json(res, 200, state_payload(app));
    });

    svr.Get("/api/config", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        send_json(res, 200, config_payload(app));
    });

    svr.Post("/api/select", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.select_config(body.value("path", ""), err)) {
            send_error(res, 400, err);
            return;
        }
        send_json(res, 200, config_payload(app));
    });

    svr.Get("/api/export/llama-swap", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        res.set_header("Content-Disposition", "attachment; filename=\"llama-swap.yaml\"");
        res.set_content(app.config.llama_swap_yaml(), "application/x-yaml; charset=utf-8");
    });

    svr.Get("/api/models", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        send_json(res, 200, json{{"data_dir", app.config.data_dir()},
                                 {"models", app.config.models()}});
    });

    svr.Post("/api/models/select", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.select_model(body.value("id", ""), err)) {
            send_error(res, 400, err);
            return;
        }
        send_json(res, 200, config_payload(app));
    });

    svr.Post("/api/data-dir", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.set_data_dir(body.value("path", ""), err)) {
            send_error(res, 400, err);
            return;
        }
        send_json(res, 200, config_payload(app));
    });

    svr.Post("/api/config/save-as", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.save_as(body.value("name", ""), err)) {
            send_error(res, 400, err);
            return;
        }
        send_json(res, 200, config_payload(app));
    });

    svr.Post("/api/user-args/merge", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        std::string err;
        if (!app.config.merge_user_args_into_config(err)) {
            send_error(res, 400, err);
            return;
        }
        send_json(res, 200, config_payload(app));
    });

    svr.Post("/api/config", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.apply(body, err)) {
            send_error(res, 400, err);
            return;
        }
        send_json(res, 200, config_payload(app));
    });

    svr.Post("/api/start", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.start_preflight(err)) { send_error(res, 400, err); return; }
        bool lazy = body.value("lazy", false);
        if (!app.process.start(lazy, false, err)) { send_error(res, 500, err); return; }
        send_json(res, 200, json{{"ok", true}, {"process", app.process.status()}});
    });

    svr.Post("/api/stop", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        std::string err;
        if (!app.process.stop(true, err)) { send_error(res, 409, err); return; }
        send_json(res, 200, json{{"ok", true}, {"process", app.process.status()}});
    });

    svr.Post("/api/restart", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.start_preflight(err)) { send_error(res, 400, err); return; }
        if (!app.process.restart(body.value("lazy", false), err)) {
            send_error(res, 500, err);
            return;
        }
        send_json(res, 200, json{{"ok", true}, {"process", app.process.status()}});
    });

    svr.Post("/api/load", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        std::string err;
        if (!app.process.load_model(err)) { send_error(res, 409, err); return; }
        send_json(res, 200, json{{"ok", true}});
    });

    svr.Post("/api/unload", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        std::string err;
        if (!app.process.unload_model(err)) { send_error(res, 409, err); return; }
        send_json(res, 200, json{{"ok", true}});
    });

    svr.Get("/api/logs", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        int lines = 300;
        if (req.has_param("lines")) {
            try { lines = std::clamp(std::stoi(req.get_param_value("lines")), 20, 1000); } catch (...) {}
        }
        std::string path = app.process.log_path();
        if (path.empty()) path = app.config.log_path();
        auto v = util::tail_lines(path, lines, 512 * 1024);
        send_json(res, 200, json{{"path", path}, {"lines", v}});
    });

    svr.Get("/api/backups", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        send_json(res, 200, json{{"backups", app.config.backups()}});
    });

    svr.Post("/api/backups/restore", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string err;
        if (!app.config.restore_backup(body.value("path", ""), err)) {
            send_error(res, 400, err);
            return;
        }
        send_json(res, 200, config_payload(app));
    });

    svr.Get("/api/command", [&](const httplib::Request& req, httplib::Response& res) {
        if (!host_ok(req)) { send_error(res, 403, "只接受本机 Host"); return; }
        send_json(res, 200, app.config.command_preview());
    });

    svr.Post("/api/autoconfig", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        send_json(res, 200, run_auto_config(app));
    });

    svr.Post("/api/api-key", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        std::string key = app.config.api_key();
        if (key.empty()) { send_error(res, 404, "尚未设置 API Key"); return; }
        send_json(res, 200, json{{"ok", true}, {"api_key", key}});
    });

    svr.Post("/api/browse", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string kind = body.value("kind", "file");
        std::string title = body.value("title", kind == "folder" ? "选择目录" : "选择文件");
        std::string current = body.value("current", "");
        std::wstring title_w = util::widen(title);
        HWND owner = GetForegroundWindow();
        std::wstring selected;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (kind == "folder") {
            BROWSEINFOW bi{};
            bi.hwndOwner = owner;
            bi.lpszTitle = title_w.c_str();
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH * 4]{};
                if (SHGetPathFromIDListW(pidl, path)) selected = path;
                CoTaskMemFree(pidl);
            }
        } else {
            OPENFILENAMEW ofn{};
            wchar_t file[32768]{};
            if (!current.empty()) {
                auto w = util::widen(current);
                wcsncpy_s(file, w.c_str(), _TRUNCATE);
            }
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = owner;
            ofn.lpstrFile = file;
            ofn.nMaxFile = (DWORD)std::size(file);
            ofn.lpstrTitle = title_w.c_str();
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) selected = file;
        }
        CoUninitialize();
        if (selected.empty()) { send_json(res, 200, json{{"ok", true}, {"path", ""}}); return; }
        send_json(res, 200, json{{"ok", true}, {"path", util::narrow(selected)}});
    });

    svr.Post("/api/open", [&](const httplib::Request& req, httplib::Response& res) {
        if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        std::string target = body.value("target", "");
        if (target == "config") {
            util::open_path(app.config.config_path());
        } else if (target == "log") {
            std::string p = app.process.log_path();
            if (p.empty()) p = app.config.log_path();
            util::open_path(p);
        } else if (target == "backups") {
            util::open_path(app.config.root() + "\\config-backups");
        } else if (target == "ui") {
            util::open_url("http://127.0.0.1:" + std::to_string(app.ui_port) + "/");
        } else {
            send_error(res, 400, "未知的打开目标");
            return;
        }
        send_json(res, 200, json{{"ok", true}});
    });

    svr.Post("/api/quit", [&](const httplib::Request& req, httplib::Response& res) {
        bool control = !app.config.control_token().empty() &&
                       req.get_header_value("X-Control-Token") == app.config.control_token();
        if (!control) {
            if (!validate_post(req, res) || !check_csrf(req, res, app.csrf)) return;
        }
        bool ok = false;
        json body = parse_body(req, res, ok);
        if (!ok) return;
        if (body.value("stop_model", true)) {
            std::string err;
            app.process.stop(true, err);
        }
        send_json(res, 200, json{{"ok", true}});
        httplib::Server* s = &svr;
        std::thread([s] {
            Sleep(300);
            s->stop();
        }).detach();
    });

    if (!daemon || open_browser) {
        std::thread([&app] {
            Sleep(700);
            util::open_url("http://127.0.0.1:" + std::to_string(app.ui_port) + "/");
        }).detach();
    }

    bool listening = svr.listen("127.0.0.1", app.ui_port);
    if (!listening) {
        std::string probe_err;
        auto probe = util::http_get_json(app.ui_port, "/api/health", "", 800, nullptr, &probe_err);
        if (probe && probe->value("app", "") == "strata-console") {
            util::open_url("http://127.0.0.1:" + std::to_string(app.ui_port) + "/");
        } else {
            MessageBoxW(nullptr,
                        util::widen("无法监听 127.0.0.1:" + std::to_string(app.ui_port) +
                                    "\n端口可能被占用。").c_str(),
                        L"Strata Console", MB_ICONERROR);
        }
    }

    app.process.shutdown();
    app.status.stop();
    if (singleton) CloseHandle(singleton);
    WSACleanup();
    return 0;
}

