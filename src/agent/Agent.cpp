#include "agent/Agent.h"

#include "agent/Input.h"
#include "common/Log.h"

#include <nlohmann/json.hpp>

#include <objbase.h>
#include <shlwapi.h>


#include <chrono>
#include <string>

#pragma comment(lib, "ole32.lib")

namespace remote_assist {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                     static_cast<int>(w.size()), nullptr, 0,
                                     nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

Agent::~Agent() {
    Stop();
}

std::string Agent::WebDirFromExe() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe = buf;
    const size_t pos = exe.find_last_of(L"\\/");
    std::wstring dir = (pos != std::wstring::npos) ? exe.substr(0, pos) : exe;
    dir += L"\\web";
    if (!PathFileExistsW(dir.c_str())) {
        // 退回到 share/remote-assist/web(便于 install 部署形态)
        std::wstring alt = dir + L"\\..\\share\\remote-assist\\web";
        wchar_t full[MAX_PATH] = {};
        if (GetFullPathNameW(alt.c_str(), MAX_PATH, full, nullptr)) {
            if (PathFileExistsW(full)) {
                return WideToUtf8(full);
            }
        }
    }
    return WideToUtf8(dir);
}

int Agent::Run() {
    log::Init(LogDir());
    log::Info("agent starting");

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);


    cfg_ = LoadOrCreateConfig();
    log::Info("agent config port=" + std::to_string(cfg_.port) +
              " fps=" + std::to_string(cfg_.fps));

    if (!desktop_.Bind()) {
        log::Error("agent: bind desktop failed");
    }
    if (!capture_.Init()) {
        log::Error("agent: capture init failed");
    }

    server_.SetWebDir(WebDirFromExe());
    server_.SetAuthVerifier([this](const std::string& token) {
        return VerifyPassword(cfg_, token);
    });
    server_.SetCfgProvider([this]() { return MakeCfgJson(); });
    server_.SetOnMessage([this](const std::string& msg) { OnMessage(msg); });

    if (!server_.Start("0.0.0.0", cfg_.port)) {
        log::Error("agent: server start failed");
    }

    captureThread_ = std::thread(&Agent::CaptureLoop, this);

    // 主线程等待停止信号(MVP:轮询 stop_ 标志,后续可改事件)。
    while (!stop_.load()) {
        Sleep(200);
    }

    Stop();
    CoUninitialize();
    log::Info("agent exit");
    return 0;
}

void Agent::Stop() {
    if (stop_.exchange(true)) {
        return;
    }
    server_.Stop();
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (encoder_) {
        encoder_->Release();
        encoder_.reset();
    }
}

void Agent::CaptureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int targetMs = 1000 / std::max(1, cfg_.fps);
    while (!stop_.load()) {
        const auto t0 = std::chrono::steady_clock::now();

        // 跟随桌面切换(锁屏↔解锁、Winlogon↔Default)。
        desktop_.CheckRebind();

        CapturedFrame frame;
        if (!capture_.CaptureFrame(frame)) {
            static int failCount = 0;
            if (++failCount % 300 == 1) {  // 每 10s(30fps*300)打一次,避免刷屏
                log::Warn("capture frame failed, count=" + std::to_string(failCount));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
            continue;
        }

        if (!encoder_ || frame.width != deskWidth_ || frame.height != deskHeight_) {
            deskWidth_ = frame.width;
            deskHeight_ = frame.height;
            encoderReady_ = false;
            encoder_ = std::make_unique<EncoderMf>();
            if (!encoder_->Init(frame.width, frame.height, cfg_.fps, cfg_.bitrate)) {
                log::Error("encoder init failed for " + std::to_string(frame.width) +
                           "x" + std::to_string(frame.height));
                encoder_.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
                continue;
            }
            encoderReady_ = true;
            // 尺寸/编码器变化后重新下发一次 cfg。
            server_.Broadcaster().BroadcastText(MakeCfgJson());
        }

        if (encoderReady_ && !frame.data.empty()) {
            std::vector<EncodedChunk> chunks;
            if (encoder_->Encode(frame.data.data(), chunks)) {
                for (const auto& c : chunks) {
                    server_.Broadcaster().BroadcastBinary(
                        reinterpret_cast<const char*>(c.data.data()), c.data.size());
                }
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const int remain = targetMs - static_cast<int>(used);
        if (remain > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remain));
        }
    }
    CoUninitialize();
}

std::string Agent::MakeCfgJson() const {
    nlohmann::json j;
    j["t"] = "cfg";
    j["codec"] = encoder_ ? encoder_->CodecString() : std::string("jpeg");
    j["w"] = deskWidth_;
    j["h"] = deskHeight_;
    j["fps"] = cfg_.fps;
    auto mons = nlohmann::json::array();
    for (const auto& m : capture_.Monitors()) {
        mons.push_back({{"index", m.index}, {"name", m.name}, {"w", m.w}, {"h", m.h}});
    }
    j["monitors"] = mons;
    return j.dump();
}

void Agent::OnMessage(const std::string& msg) {
    auto j = nlohmann::json::parse(msg, nullptr, false);
    if (!j.is_object()) {
        return;
    }
    const std::string t = j.value("t", std::string());
    if (t == "key") {
        const USHORT sc = static_cast<USHORT>(j.value("sc", 0));
        const bool down = j.value("down", false);
        Input::SendKey(sc, down);
    } else if (t == "mouse") {
        const double x = j.value("x", 0.0);
        const double y = j.value("y", 0.0);
        const std::string btn = j.value("btn", std::string());
        const bool down = j.value("down", false);
        Input::SendMouseAbs(x, y);
        Input::SendMouseButton(btn, down);
    } else if (t == "move") {
        const double x = j.value("x", 0.0);
        const double y = j.value("y", 0.0);
        Input::SendMouseAbs(x, y);
    } else if (t == "wheel") {
        const int delta = j.value("delta", 0);
        Input::SendWheel(delta);
    } else if (t == "monitor") {
        const int idx = j.value("index", -1);
        capture_.SetMonitor(idx);
        log::Info("monitor switched to idx=" + std::to_string(idx));
        deskWidth_ = 0;
        deskHeight_ = 0;
    }
}

}  // namespace remote_assist
