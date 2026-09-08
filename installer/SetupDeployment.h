#pragma once

#include <Windows.h>
#include <winrt/base.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace unfurl::setup {

using Progress = std::function<void(std::wstring const&)>;

struct Session {
    Session();
    ~Session();
    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    std::filesystem::path directory;
    std::atomic_bool cancelled{false};
    bool keep_feed{};
};

struct Bootstrap {
    explicit Bootstrap(Session const& session);
    ~Bootstrap();
    Bootstrap(Bootstrap const&) = delete;
    Bootstrap& operator=(Bootstrap const&) = delete;
    HRESULT Initialize();

  private:
    HMODULE module_{};
    bool initialized_{};
};

std::wstring error_text(winrt::hresult_error const& error);
bool certificate_trusted();
int trust_certificate();
void request_certificate_trust(Session& session);
void prepare_dependencies(Session& session, Progress const& progress);
bool app_installer_available();
void open_app_installer(Session& session);
void run_window(std::shared_ptr<Session> session);
int prepare_runtime_dialog(std::shared_ptr<Session> const& session);

} // namespace unfurl::setup
