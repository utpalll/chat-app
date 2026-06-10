// =============================================================================
//  Chat Server - Discord Facelift Edition
// =============================================================================
#include "common.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

enum class ClientKind {
    RawTcp,
    ServerSentEvents,
};

struct ClientInfo {
    std::string name;
    ClientKind  kind;
};

std::map<int, ClientInfo> g_clients;
std::mutex                g_clients_mutex;
std::atomic<bool>         g_running{true};

bool send_all(int fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = ::send(fd, data.data() + total_sent,
                              data.size() - total_sent, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

std::string sse_payload(const std::string& message) {
    std::string payload = message;
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r')) {
        payload.pop_back();
    }

    std::string framed = "data: ";
    for (char ch : payload) {
        if (ch == '\n') {
            framed += "\ndata: ";
        } else if (ch != '\r') {
            framed += ch;
        }
    }
    framed += "\n\n";
    return framed;
}

bool send_client_message(int fd, const ClientInfo& client,
                         const std::string& message) {
    if (client.kind == ClientKind::ServerSentEvents) {
        return send_all(fd, sse_payload(message));
    }
    return send_all(fd, message);
}

void broadcast(const std::string& message, int exclude_fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        int fd = it->first;
        if (fd != exclude_fd) {
            if (!send_client_message(fd, it->second, message)) {
                ::close(fd);
                it = g_clients.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void register_client(int fd, const std::string& name, ClientKind kind) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients[fd] = ClientInfo{name, kind};
}

std::string unregister_client(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    auto it = g_clients.find(fd);
    if (it == g_clients.end()) {
        return {};
    }
    std::string name = it->second.name;
    g_clients.erase(it);
    return name;
}

size_t client_count() {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    return g_clients.size();
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

std::string url_decode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            std::string hex = value.substr(i + 1, 2);
            decoded.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else if (value[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::string form_value(const std::string& data, const std::string& key) {
    std::istringstream pairs(data);
    std::string pair;
    while (std::getline(pairs, pair, '&')) {
        size_t equals = pair.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        if (url_decode(pair.substr(0, equals)) == key) {
            return url_decode(pair.substr(equals + 1));
        }
    }
    return {};
}

std::string query_value(const std::string& target, const std::string& key) {
    size_t question = target.find('?');
    if (question == std::string::npos) {
        return {};
    }
    return form_value(target.substr(question + 1), key);
}

int content_length(const std::string& headers) {
    std::istringstream lines(headers);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string lower = line;
        for (char& ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (starts_with(lower, "content-length:")) {
            return std::stoi(line.substr(line.find(':') + 1));
        }
    }
    return 0;
}

// =============================================================================
//  DISCORD THEMED HTML/CSS UI 
// =============================================================================
std::string chat_page() {
    return R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Family Chat</title>
  
  <style>
    :root {
      --bg-0: #0b0f19;
      --bg-1: #111827;
      --bg-2: #171b2a;
      --bg-3: #1f2436;
      --panel: rgba(17, 24, 39, 0.78);
      --panel-strong: rgba(11, 15, 25, 0.92);
      --line: rgba(255, 255, 255, 0.08);
      --line-strong: rgba(255, 255, 255, 0.14);
      --text: #eef2ff;
      --muted: #9aa3b2;
      --muted-2: #7b8496;
      --brand: #7c5cff;
      --brand-2: #8b5cf6;
      --brand-3: #22d3ee;
      --brand-4: #ff5ea8;
      --success: #2dd4bf;
      --danger: #fb7185;
      --shadow: 0 28px 80px rgba(0, 0, 0, 0.45);
      --shadow-soft: 0 10px 35px rgba(0, 0, 0, 0.28);
      --radius-xl: 28px;
      --radius-lg: 22px;
      --radius-md: 16px;
      --radius-sm: 12px;
      --sidebar-w: 270px;
      --header-h: 66px;
      --composer-h: 92px;
      --channel-h: 50px;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }
    html, body { width: 100%; height: 100%; }

    body {
      min-height: 100vh;
      overflow: hidden;
      color: var(--text);
      font-family: Inter, "Segoe UI", "Helvetica Neue", Arial, sans-serif;
      background:
        radial-gradient(circle at 10% 10%, rgba(124, 92, 255, 0.22), transparent 28%),
        radial-gradient(circle at 90% 18%, rgba(34, 211, 238, 0.18), transparent 24%),
        radial-gradient(circle at 75% 92%, rgba(255, 94, 168, 0.18), transparent 26%),
        linear-gradient(135deg, #060816 0%, #0b1020 42%, #080b14 100%);
      position: relative;
    }

    body::before, body::after {
      content: "";
      position: fixed;
      inset: auto;
      pointer-events: none;
      filter: blur(50px);
      opacity: 0.7;
      z-index: 0;
    }
    body::before {
      width: 32rem; height: 32rem; left: -8rem; top: -6rem;
      background: radial-gradient(circle, rgba(124, 92, 255, 0.32), transparent 68%);
    }
    body::after {
      width: 26rem; height: 26rem; right: -7rem; bottom: -6rem;
      background: radial-gradient(circle, rgba(34, 211, 238, 0.28), transparent 70%);
    }

    #app {
      position: relative;
      z-index: 1;
      width: 100vw;
      height: 100vh;
      display: flex;
      overflow: hidden;
      border: 1px solid rgba(255, 255, 255, 0.07);
      background: linear-gradient(180deg, rgba(11, 15, 25, 0.82), rgba(11, 15, 25, 0.72));
      box-shadow: var(--shadow);
      backdrop-filter: blur(20px);
      border-radius: 0;
    }

    #app::before {
      content: "";
      position: absolute;
      inset: 0;
      pointer-events: none;
      background:
        linear-gradient(120deg, rgba(124, 92, 255, 0.08), transparent 30%),
        linear-gradient(300deg, rgba(34, 211, 238, 0.06), transparent 35%);
      z-index: 0;
    }

    #app > * { position: relative; z-index: 1; }

    #app:not(.joined) .sidebar { display: none; }
    #app:not(.joined) .main-chat-area {
      width: 100vw; height: 100vh;
      display: flex; justify-content: center; align-items: center;
      background:
        radial-gradient(circle at 50% 35%, rgba(124, 92, 255, 0.08), transparent 34%),
        radial-gradient(circle at 50% 100%, rgba(255, 94, 168, 0.08), transparent 28%),
        linear-gradient(180deg, rgba(9, 12, 22, 0.96), rgba(6, 8, 16, 0.92));
    }
   #app:not(.joined) .main-chat-area {
      width: 100vw; height: 100vh;
      display: flex; justify-content: center; align-items: center;
      background:
        radial-gradient(circle at 50% 35%, rgba(124, 92, 255, 0.08), transparent 34%),
        radial-gradient(circle at 50% 100%, rgba(255, 94, 168, 0.08), transparent 28%),
        linear-gradient(180deg, rgba(9, 12, 22, 0.96), rgba(6, 8, 16, 0.92));
    }
    #app:not(.joined) header, #app:not(.joined) #messages, #app:not(.joined) #composer { display: none; }

    #app.joined #joinPanel { 
      display: none; 
    }}

    #app.joined .sidebar {
      width: var(--sidebar-w);
      background: linear-gradient(180deg, rgba(19, 24, 38, 0.92), rgba(14, 18, 29, 0.96));
      display: flex;
      flex-direction: column;
      border-right: 1px solid var(--line);
      box-shadow: inset -1px 0 0 rgba(255, 255, 255, 0.03);
    }

    .sidebar-header {
      height: 78px; padding: 0 18px;
      display: flex; align-items: center; gap: 12px;
      border-bottom: 1px solid var(--line);
      background:
        linear-gradient(135deg, rgba(124, 92, 255, 0.16), rgba(34, 211, 238, 0.08)),
        rgba(255, 255, 255, 0.02);
      position: relative;
    }
    .sidebar-header::before {
      content: ""; width: 12px; height: 12px; border-radius: 999px;
      background: linear-gradient(135deg, var(--brand-3), var(--brand));
      box-shadow: 0 0 0 6px rgba(124, 92, 255, 0.1);
      flex: 0 0 auto;
    }
    .sidebar-header::after {
      content: "";
      position: absolute; left: 18px; right: 18px; bottom: -1px; height: 1px;
      background: linear-gradient(90deg, transparent, rgba(124, 92, 255, 0.75), rgba(34, 211, 238, 0.7), transparent);
      opacity: 0.8;
    }
    .sidebar-header h3 {
      font-size: 15px; font-weight: 800; letter-spacing: 0.2px; color: #fff; line-height: 1;
    }

    .sidebar-channels { padding: 14px 10px; flex: 1; overflow-y: auto; }
    .sidebar-channels::-webkit-scrollbar, #messages::-webkit-scrollbar { width: 10px; }
    .sidebar-channels::-webkit-scrollbar-track, #messages::-webkit-scrollbar-track { background: transparent; }
    .sidebar-channels::-webkit-scrollbar-thumb, #messages::-webkit-scrollbar-thumb {
      background: linear-gradient(180deg, rgba(124, 92, 255, 0.35), rgba(34, 211, 238, 0.28));
      border-radius: 999px;
      border: 2px solid transparent;
      background-clip: padding-box;
    }

    .channel-item {
      height: var(--channel-h);
      display: flex; align-items: center; gap: 10px;
      padding: 0 14px;
      border-radius: 16px;
      color: var(--muted);
      font-weight: 700;
      font-size: 15px;
      cursor: pointer;
      margin-bottom: 8px;
      transition: transform 0.2s ease, background 0.2s ease, color 0.2s ease, box-shadow 0.2s ease;
      background: transparent;
    }
    .channel-item:hover {
      transform: translateX(4px);
      background: rgba(255, 255, 255, 0.05);
      color: #fff;
      box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.04);
    }
    .channel-item.active {
      color: #fff;
      background:
        linear-gradient(135deg, rgba(124, 92, 255, 0.18), rgba(34, 211, 238, 0.08)),
        rgba(255, 255, 255, 0.05);
      box-shadow: inset 0 0 0 1px rgba(124, 92, 255, 0.28), 0 10px 24px rgba(124, 92, 255, 0.12);
    }

    .hash {
      font-size: 18px;
      color: #a5b4fc;
      font-weight: 900;
      line-height: 1;
      text-shadow: 0 0 16px rgba(124, 92, 255, 0.45);
    }

    #app.joined .main-chat-area {
      flex: 1;
      display: flex;
      flex-direction: column;
      height: 100vh;
      background:
        radial-gradient(circle at 0% 0%, rgba(124, 92, 255, 0.08), transparent 30%),
        radial-gradient(circle at 100% 12%, rgba(255, 94, 168, 0.06), transparent 24%),
        linear-gradient(180deg, rgba(16, 20, 32, 0.92), rgba(11, 15, 25, 0.96));
      position: relative;
    }
    #app.joined .main-chat-area::before {
      content: "";
      position: absolute;
      inset: 0;
      pointer-events: none;
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.02), transparent 10%),
        linear-gradient(90deg, rgba(255, 255, 255, 0.015) 1px, transparent 1px),
        linear-gradient(rgba(255, 255, 255, 0.015) 1px, transparent 1px);
      background-size: auto, 48px 48px, 48px 48px;
      mask-image: linear-gradient(180deg, rgba(0, 0, 0, 0.85), transparent 95%);
      opacity: 0.35;
    }

    header {
      height: var(--header-h);
      padding: 0 18px;
      display: flex; align-items: center; justify-content: space-between;
      border-bottom: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(20, 25, 38, 0.95), rgba(17, 21, 34, 0.78));
      backdrop-filter: blur(18px);
      box-shadow: 0 8px 30px rgba(0, 0, 0, 0.18);
    }
    .header-left {
      font-size: 16px; font-weight: 800; color: #fff;
      display: flex; align-items: center; gap: 10px; letter-spacing: 0.1px;
    }
    .header-left .hash { font-size: 19px; }

    #status {
      font-size: 12px;
      color: var(--muted);
      display: inline-flex; align-items: center; gap: 8px;
      font-weight: 800;
      text-transform: uppercase;
      letter-spacing: 0.16em;
      padding: 9px 12px;
      border-radius: 999px;
      background: rgba(255, 255, 255, 0.04);
      border: 1px solid rgba(255, 255, 255, 0.06);
    }
    #status::before {
      content: "";
      width: 8px; height: 8px; border-radius: 50%;
      background: linear-gradient(180deg, #fb7185, #ef4444);
      box-shadow: 0 0 0 6px rgba(251, 113, 133, 0.09);
      flex: 0 0 auto;
    }
    #status.connected {
      color: #d8fff6;
      border-color: rgba(45, 212, 191, 0.18);
      background: rgba(45, 212, 191, 0.09);
    }
    #status.connected::before {
      background: linear-gradient(180deg, #34d399, #10b981);
      box-shadow: 0 0 0 6px rgba(16, 185, 129, 0.12);
    }

    #joinPanel {
      display: grid; place-items: center;
      width: 100%; height: 100%;
      padding: 24px;
    }

    .join-box {
      width: min(520px, 100%);
      padding: 34px 34px 30px;
      border-radius: 30px;
      background: linear-gradient(180deg, rgba(22, 27, 43, 0.92), rgba(13, 17, 28, 0.92));
      border: 1px solid rgba(255, 255, 255, 0.08);
      box-shadow: var(--shadow);
      position: relative;
      overflow: hidden;
    }
    .join-box::before {
      content: "";
      position: absolute;
      inset: -2px;
      background:
        radial-gradient(circle at 15% 0%, rgba(124, 92, 255, 0.35), transparent 28%),
        radial-gradient(circle at 100% 12%, rgba(34, 211, 238, 0.22), transparent 26%),
        radial-gradient(circle at 70% 110%, rgba(255, 94, 168, 0.18), transparent 26%);
      opacity: 0.8;
      pointer-events: none;
    }
    .join-box > * { position: relative; z-index: 1; }
    .join-box h2 {
      color: #fff;
      font-size: clamp(30px, 4vw, 40px);
      line-height: 1.05;
      margin-bottom: 10px;
      font-weight: 900;
      letter-spacing: -0.04em;
      text-align: left;
    }
    .join-subtitle {
      color: var(--muted);
      font-size: 15px;
      margin-bottom: 22px;
      line-height: 1.6;
      text-align: left;
    }
    .join-row { display: flex; flex-direction: column; text-align: left; gap: 10px; }
    .join-row label {
      color: #c9d1e6;
      font-size: 12px;
      font-weight: 800;
      letter-spacing: 0.18em;
      text-transform: uppercase;
      margin-left: 2px;
    }
    .join-box input {
      width: 100%;
      background: rgba(8, 12, 20, 0.88);
      border: 1px solid rgba(255, 255, 255, 0.09);
      border-radius: 18px;
      color: var(--text);
      padding: 15px 16px;
      font-size: 15px;
      outline: none;
      transition: border-color 0.2s ease, box-shadow 0.2s ease, transform 0.2s ease, background 0.2s ease;
      box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.03);
    }
    .join-box input::placeholder, #message::placeholder { color: #8a93a8; }
    .join-box input:focus {
      border-color: rgba(124, 92, 255, 0.75);
      box-shadow: 0 0 0 4px rgba(124, 92, 255, 0.18), 0 10px 30px rgba(124, 92, 255, 0.12);
      transform: translateY(-1px);
      background: rgba(10, 14, 23, 0.98);
    }
    .join-box button {
      margin-top: 8px;
      border: none;
      border-radius: 18px;
      padding: 14px 18px;
      font-size: 15px;
      font-weight: 900;
      letter-spacing: 0.01em;
      cursor: pointer;
      color: white;
      background: linear-gradient(135deg, var(--brand), var(--brand-3) 55%, var(--brand-4));
      box-shadow: 0 16px 32px rgba(124, 92, 255, 0.28), 0 10px 24px rgba(34, 211, 238, 0.14);
      transition: transform 0.2s ease, filter 0.2s ease, box-shadow 0.2s ease;
    }
    .join-box button:hover {
      transform: translateY(-1px);
      filter: brightness(1.06);
      box-shadow: 0 18px 40px rgba(124, 92, 255, 0.34), 0 14px 28px rgba(34, 211, 238, 0.18);
    }

    #messages {
      flex: 1;
      overflow-y: auto;
      padding: 20px 12px 16px;
      display: flex;
      flex-direction: column;
      gap: 2px;
      scroll-behavior: smooth;
    }

    .message {
      width: 100%;
      padding: 14px 14px 14px 18px;
      border-radius: 18px;
      transition: background 0.2s ease, transform 0.2s ease, box-shadow 0.2s ease;
      border: 1px solid transparent;
    }
    .message:hover {
      background: rgba(255, 255, 255, 0.03);
      border-color: rgba(255, 255, 255, 0.05);
      transform: translateY(-1px);
    }
    .message.own {
      background: linear-gradient(135deg, rgba(124, 92, 255, 0.11), rgba(34, 211, 238, 0.06)), rgba(255, 255, 255, 0.02);
      border-color: rgba(124, 92, 255, 0.14);
      box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.02);
    }
    .message.own .meta::after {
      content: "YOU";
      font-size: 10px;
      font-weight: 900;
      letter-spacing: 0.16em;
      color: #d8e4ff;
      margin-left: 10px;
      padding: 4px 8px;
      border-radius: 999px;
      background: rgba(124, 92, 255, 0.17);
      border: 1px solid rgba(124, 92, 255, 0.22);
      vertical-align: middle;
    }

    .meta {
      font-size: 14px;
      font-weight: 800;
      color: #fff;
      margin-bottom: 6px;
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .meta::before {
      content: "";
      width: 10px; height: 10px; border-radius: 999px;
      background: linear-gradient(135deg, var(--brand-3), var(--brand));
      box-shadow: 0 0 0 4px rgba(124, 92, 255, 0.08);
      flex: 0 0 auto;
    }

    .bubble {
      font-size: 15px;
      color: var(--text);
      line-height: 1.6;
      word-break: break-word;
      white-space: pre-wrap;
      padding-left: 16px;
      border-left: 2px solid rgba(124, 92, 255, 0.18);
    }

    .system {
      padding: 10px 14px;
      margin: 6px 0;
      border-radius: 16px;
      font-size: 14px;
      color: #c3c9d7;
      display: flex;
      align-items: center;
      gap: 10px;
      line-height: 1.5;
      background: rgba(255, 255, 255, 0.03);
      border: 1px solid rgba(255, 255, 255, 0.05);
    }
    .system::before {
      content: "✦";
      color: #22d3ee;
      font-size: 13px;
      text-shadow: 0 0 12px rgba(34, 211, 238, 0.35);
      flex: 0 0 auto;
    }

    #composer {
      padding: 18px 16px 20px;
      background: linear-gradient(180deg, rgba(13, 17, 28, 0.16), rgba(13, 17, 28, 0.82));
      display: flex;
      align-items: center;
      gap: 12px;
      border-top: 1px solid var(--line);
      backdrop-filter: blur(18px);
      min-height: var(--composer-h);
    }
    #message {
      flex: 1;
      min-width: 0;
      background: linear-gradient(180deg, rgba(8, 12, 20, 0.96), rgba(12, 16, 26, 0.92));
      border: 1px solid rgba(255, 255, 255, 0.07);
      border-radius: 22px;
      color: var(--text);
      padding: 16px 18px;
      font-size: 15px;
      outline: none;
      transition: border-color 0.2s ease, box-shadow 0.2s ease, transform 0.2s ease;
      box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.03), 0 10px 30px rgba(0, 0, 0, 0.18);
    }
    #message:focus {
      border-color: rgba(34, 211, 238, 0.55);
      box-shadow: 0 0 0 4px rgba(34, 211, 238, 0.12), 0 14px 34px rgba(34, 211, 238, 0.08);
      transform: translateY(-1px);
    }
    #send {
      border: none;
      border-radius: 20px;
      padding: 15px 20px;
      min-width: 104px;
      font-weight: 900;
      letter-spacing: 0.01em;
      cursor: pointer;
      font-size: 14px;
      color: white;
      background: linear-gradient(135deg, var(--brand), var(--brand-2) 35%, var(--brand-4));
      box-shadow: 0 16px 32px rgba(124, 92, 255, 0.22), 0 10px 22px rgba(255, 94, 168, 0.12);
      transition: transform 0.2s ease, filter 0.2s ease, box-shadow 0.2s ease, opacity 0.2s ease;
    }
    #send:hover:not(:disabled) {
      transform: translateY(-1px);
      filter: brightness(1.05);
      box-shadow: 0 18px 38px rgba(124, 92, 255, 0.28), 0 14px 26px rgba(255, 94, 168, 0.16);
    }
    #send:disabled { opacity: 0.45; cursor: not-allowed; box-shadow: none; filter: saturate(0.7); }

    @media (max-width: 900px) {
      #app.joined .sidebar { width: 224px; }
      .join-box { padding: 28px 24px 24px; }
      #composer { padding: 14px 12px 16px; }
    }

    @media (max-width: 720px) {
      #app { border-radius: 0; }
      #app.joined .sidebar { display: none; }
      header { padding: 0 14px; }
      #status { letter-spacing: 0.12em; padding: 8px 10px; }
      .join-box { border-radius: 24px; }
      .message { padding: 12px 12px 12px 14px; }
      .bubble { padding-left: 12px; }
      #send { min-width: 88px; padding-inline: 16px; }
    }
  </style>

</head>
<body>
  <main id="app">
    <div class="sidebar">
      <div class="sidebar-header">
        <h3>Family Server</h3>
      </div>
      <div class="sidebar-channels">
        <div class="channel-item active">
          <span class="hash">#</span> general
        </div>
      </div>
    </div>
    
    <div class="main-chat-area">
      <header>
        <div class="header-left">
          <span class="hash">#</span> general
        </div>
        <div id="status">Waiting to join</div>
      </header>
      
      <section id="joinPanel">
        <form id="joinForm" class="join-box">
          <h2>Welcome back!</h2>
          <div class="join-subtitle">We're so excited to see you again!</div>
          <div class="join-row">
            <label>ENTER A DISPLAY NAME</label>
            <input id="name" autocomplete="name" placeholder="What should we call you?" maxlength="32">
            <button type="submit">Join Room</button>
          </div>
        </form>
      </section>
      
      <section id="messages" aria-live="polite"></section>
      
      <form id="composer">
        <input id="message" autocomplete="off" placeholder="Message #general" maxlength="500">
        <button id="send" type="submit" disabled>Send</button>
      </form>
    </div>
  </main>
  <script>
    const app = document.querySelector("#app");
    const joinForm = document.querySelector("#joinForm");
    const nameInput = document.querySelector("#name");
    const messages = document.querySelector("#messages");
    const status = document.querySelector("#status");
    const input = document.querySelector("#message");
    const send = document.querySelector("#send");
    let events = null;
    let selfName = "anonymous";

    function now() {
      return new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    }

    function addMessage(text) {
      if (!text) return;
      if (text.startsWith("***")) {
        const node = document.createElement("div");
        node.className = "system";
        node.textContent = text.replaceAll("***", "").trim();
        messages.appendChild(node);
        messages.scrollTop = messages.scrollHeight;
        return;
      }

      const node = document.createElement("div");
      const splitAt = text.indexOf(": ");
      const sender = splitAt > 0 ? text.slice(0, splitAt) : "Chat";
      const body = splitAt > 0 ? text.slice(splitAt + 2) : text;
      node.className = sender === selfName ? "message own" : "message";

      const meta = document.createElement("div");
      meta.className = "meta";
      meta.textContent = `${sender} — ${now()}`;

      const bubble = document.createElement("div");
      bubble.className = "bubble";
      bubble.textContent = body;

      node.append(meta, bubble);
      messages.appendChild(node);
      messages.scrollTop = messages.scrollHeight;
    }

    function connect(name) {
      selfName = name;
      app.classList.add("joined");
      status.textContent = `Connecting as ${selfName}`;
      events = new EventSource(`/events?name=${encodeURIComponent(selfName)}`);
      events.onopen = () => {
        status.textContent = `Connected as ${selfName}`;
        status.classList.add("connected");
        send.disabled = false;
        input.focus();
      };
      events.onerror = () => {
        status.textContent = "Reconnecting";
        status.classList.remove("connected");
      };
      events.onmessage = (event) => addMessage(event.data);
    }

    joinForm.addEventListener("submit", (event) => {
      event.preventDefault();
      const chosenName = nameInput.value.trim() || "anonymous";
      connect(chosenName);
    });

    document.querySelector("#composer").addEventListener("submit", async (event) => {
      event.preventDefault();
      const message = input.value.trim();
      if (!message) return;
      input.value = "";
      try {
        await fetch("/send", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: new URLSearchParams({ name: selfName, message })
        });
      } catch (error) {
        addMessage("*** Message failed to send ***");
      }
      input.focus();
    });

    nameInput.focus();
  </script>
</body>
</html>)";
}

void send_http_response(int fd, const std::string& status,
                        const std::string& content_type,
                        const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "\r\n"
             << body;
    send_all(fd, response.str());
}

void handle_sse_client(int client_fd, const std::string& target,
                       const std::string& peer_address) {
    std::string display_name = query_value(target, "name");
    if (display_name.empty()) {
        display_name = peer_address;
    }

    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    if (!send_all(client_fd, headers)) {
        ::close(client_fd);
        return;
    }

    register_client(client_fd, display_name, ClientKind::ServerSentEvents);
    std::cout << "[server] " << display_name << " joined from "
              << peer_address << " (" << client_count() << " online)\n";

    broadcast("*** " + display_name + " has joined the chat ***\n", client_fd);
    send_all(client_fd, sse_payload("*** Welcome, " + display_name +
                                    "! You are now connected. ***\n"));

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        if (!send_all(client_fd, ": keepalive\n\n")) {
            break;
        }
    }

    std::string name = unregister_client(client_fd);
    ::close(client_fd);
    if (!name.empty()) {
        std::cout << "[server] " << name << " disconnected ("
                  << client_count() << " online)\n";
        broadcast("*** " + name + " has left the chat ***\n", -1);
    }
}

void handle_http_client(int client_fd, std::string request,
                        const std::string& peer_address) {
    while (request.find("\r\n\r\n") == std::string::npos) {
        char buffer[chat::RECV_BUFFER_SIZE];
        ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            ::close(client_fd);
            return;
        }
        request.append(buffer, static_cast<size_t>(received));
    }

    size_t header_end = request.find("\r\n\r\n");
    std::string headers = request.substr(0, header_end);
    std::string body = request.substr(header_end + 4);

    std::istringstream first_line_stream(headers);
    std::string method;
    std::string target;
    first_line_stream >> method >> target;

    int expected_body = content_length(headers);
    while (static_cast<int>(body.size()) < expected_body) {
        char buffer[chat::RECV_BUFFER_SIZE];
        ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            ::close(client_fd);
            return;
        }
        body.append(buffer, static_cast<size_t>(received));
    }

    if (method == "GET" && starts_with(target, "/events")) {
        handle_sse_client(client_fd, target, peer_address);
        return;
    }

    if (method == "HEAD") {
        send_http_response(client_fd, "200 OK", "text/plain", "");
        ::close(client_fd);
        return;
    }    

    if (method == "POST" && target == "/send") {
        std::string name = form_value(body, "name");
        std::string message = form_value(body, "message");
        if (name.empty()) {
            name = "anonymous";
        }
        if (!message.empty()) {
            broadcast(name + ": " + message + "\n", -1);
        }
        send_http_response(client_fd, "204 No Content", "text/plain", "");
        ::close(client_fd);
        return;
    }

    if (method == "OPTIONS") {
        send_http_response(client_fd, "204 No Content", "text/plain", "");
        ::close(client_fd);
        return;
    }

    send_http_response(client_fd, "200 OK", "text/html; charset=utf-8",
                       chat_page());
    ::close(client_fd);
}

void handle_client(int client_fd, std::string peer_address) {
    char   buffer[chat::RECV_BUFFER_SIZE];
    std::string inbox;
    std::string display_name;

    while (g_running.load()) {
        ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        inbox.append(buffer, static_cast<size_t>(received));

        if (display_name.empty() &&
            (starts_with(inbox, "GET ") || starts_with(inbox, "POST ") ||
             starts_with(inbox, "OPTIONS ") || starts_with(inbox, "HEAD "))) {
            handle_http_client(client_fd, inbox, peer_address);
            return;
        }

        size_t newline_pos;
        while ((newline_pos = inbox.find(chat::MESSAGE_DELIMITER)) != std::string::npos) {
            std::string line = inbox.substr(0, newline_pos);
            inbox.erase(0, newline_pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (display_name.empty()) {
                display_name = line.empty() ? peer_address : line;
                register_client(client_fd, display_name, ClientKind::RawTcp);

                std::cout << "[server] " << display_name << " joined from "
                          << peer_address << " (" << client_count()
                          << " online)\n";

                broadcast("*** " + display_name + " has joined the chat ***\n",
                          client_fd);
                send_all(client_fd,
                         "*** Welcome, " + display_name +
                         "! You are now connected. ***\n");
            } else {
                if (line.empty()) {
                    continue;
                }
                std::string formatted = display_name + ": " + line + "\n";
                std::cout << formatted;
                broadcast(formatted, client_fd);
            }
        }
    }

    std::string name = unregister_client(client_fd);
    ::close(client_fd);

    if (!name.empty()) {
        std::cout << "[server] " << name << " disconnected ("
                  << client_count() << " online)\n";
        broadcast("*** " + name + " has left the chat ***\n", -1);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    uint16_t port = chat::DEFAULT_PORT;
    if (const char* env_port = std::getenv("PORT")) {
        if (*env_port) {
            port = static_cast<uint16_t>(std::stoi(env_port));
        }
    }
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(port);

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&server_addr),
               sizeof(server_addr)) < 0) {
        std::perror("bind");
        ::close(listen_fd);
        return 1;
    }

    if (::listen(listen_fd, chat::LISTEN_BACKLOG) < 0) {
        std::perror("listen");
        ::close(listen_fd);
        return 1;
    }

    std::cout << "[server] Chat server listening on port " << port << "\n";
    std::cout << "[server] Press Ctrl+C to stop.\n";

    while (g_running.load()) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        int client_fd = ::accept(listen_fd,
                                 reinterpret_cast<sockaddr*>(&client_addr),
                                 &client_len);
        if (client_fd < 0) {
            if (g_running.load()) {
                std::perror("accept");
            }
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::string peer = std::string(ip_str) + ":" +
                           std::to_string(ntohs(client_addr.sin_port));

        std::thread(handle_client, client_fd, peer).detach();
    }

    ::close(listen_fd);
    return 0;
}
