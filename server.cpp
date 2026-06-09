// =============================================================================
//  Chat Server
// =============================================================================
//
//  A multi-client TCP chat server written in C++ using POSIX sockets.
//
//  Architecture
//  ------------
//   * One acceptor loop (main thread) waits for incoming connections.
//   * Each accepted client is handled on its own detached worker thread.
//   * A shared, mutex-protected registry of connected clients lets the
//     server broadcast every message to all *other* connected clients.
//
//  Protocol
//  --------
//   Newline-terminated UTF-8 text. The first line a client sends is treated
//   as its display name; every subsequent line is broadcast as a chat
//   message prefixed with that name.
//
//  Build
//  -----
//   g++ -std=c++17 -pthread server.cpp -o server
//
//  Run
//  ---
//   ./server [port]
//
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

// -----------------------------------------------------------------------------
//  Connected-client registry
// -----------------------------------------------------------------------------
//
enum class ClientKind {
    RawTcp,
    ServerSentEvents,
};

struct ClientInfo {
    std::string name;
    ClientKind  kind;
};

// Maps a client socket file descriptor to that client's display name/type.
// Guarded by `g_clients_mutex` because worker threads read and write it
// concurrently.
std::map<int, ClientInfo> g_clients;
std::mutex                g_clients_mutex;

// Flips to false when the server is shutting down (e.g. on SIGINT).
std::atomic<bool> g_running{true};

// Writes the entire contents of `data` to `fd`, retrying on partial writes.
// Returns true on success, false if the socket failed.
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

// Sends `message` to every connected client except `exclude_fd`.
// Pass exclude_fd = -1 to broadcast to absolutely everyone.
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

// Adds a client to the registry.
void register_client(int fd, const std::string& name, ClientKind kind) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients[fd] = ClientInfo{name, kind};
}

// Removes a client from the registry and returns its last known name.
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

// Returns the current number of connected clients.
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

std::string chat_page() {
    return R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Family Chat</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #090a0f;
      --glass-bg: rgba(20, 25, 35, 0.65);
      --glass-border: rgba(255, 255, 255, 0.08);
      --text: #fdfdfd;
      --muted: #9ca3af;
      --accent1: #ff007a;
      --accent2: #7928ca;
      --accent3: #00d4ff;
      --bubble-other: rgba(255, 255, 255, 0.05);
      --bubble-own: linear-gradient(135deg, var(--accent2) 0%, var(--accent1) 100%);
      --system-bg: rgba(255, 255, 255, 0.03);
    }
    * { box-sizing: border-box; }
    html, body {
      margin: 0;
      min-height: 100vh;
      background-color: var(--bg);
      background-image: 
        radial-gradient(circle at 10% 20%, rgba(121, 40, 202, 0.15), transparent 40%),
        radial-gradient(circle at 90% 80%, rgba(0, 212, 255, 0.15), transparent 40%),
        radial-gradient(circle at 50% 50%, rgba(255, 0, 122, 0.05), transparent 50%);
      background-attachment: fixed;
      color: var(--text);
      font-family: 'Inter', ui-sans-serif, system-ui, -apple-system, sans-serif;
    }
    body { display: grid; place-items: center; padding: 20px; }
    
    main {
      width: min(920px, 100%);
      height: min(780px, calc(100vh - 40px));
      min-height: 560px;
      display: grid;
      grid-template-rows: auto 1fr auto;
      background: var(--glass-bg);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      border: 1px solid var(--glass-border);
      border-radius: 20px;
      overflow: hidden;
      box-shadow: 0 30px 60px rgba(0, 0, 0, 0.4), inset 0 1px 0 rgba(255,255,255,0.1);
    }
    
    header {
      display: flex; align-items: center; justify-content: space-between; gap: 16px;
      padding: 20px 24px;
      background: rgba(0, 0, 0, 0.2);
      border-bottom: 1px solid var(--glass-border);
    }
    h1 {
      margin: 0; font-size: clamp(20px, 3vw, 26px); font-weight: 700;
      background: linear-gradient(to right, var(--accent3), #fff);
      -webkit-background-clip: text; -webkit-text-fill-color: transparent;
    }
    .brand { display: flex; align-items: center; gap: 14px; min-width: 0; }
    .mark {
      width: 48px; height: 48px; border-radius: 12px;
      background: linear-gradient(135deg, var(--accent2), var(--accent1));
      color: white; display: grid; place-items: center;
      font-weight: 800; font-size: 18px; flex: 0 0 auto;
      box-shadow: 0 4px 12px rgba(121, 40, 202, 0.3);
    }
    #status {
      display: inline-flex; align-items: center; gap: 8px;
      color: var(--muted); font-size: 13px; font-weight: 500; text-transform: uppercase; letter-spacing: 0.5px;
    }
    #status::before {
      content: ""; width: 8px; height: 8px; border-radius: 50%;
      background: #ff4757; box-shadow: 0 0 10px #ff4757;
      transition: all 0.3s ease;
    }
    #status.connected::before {
      background: #2ed573; box-shadow: 0 0 10px #2ed573;
    }
    
    #joinPanel { display: grid; place-items: center; padding: 24px; }
    .join-box {
      width: min(400px, 100%); display: grid; gap: 20px; padding: 30px;
      border: 1px solid var(--glass-border); border-radius: 16px;
      background: rgba(0, 0, 0, 0.2); box-shadow: 0 8px 32px rgba(0,0,0,0.2);
    }
    .join-box h2 { margin: 0; font-size: 24px; text-align: center; font-weight: 600; }
    .join-row { display: flex; gap: 12px; flex-direction: column; }
    
    #messages {
      min-height: 0; padding: 24px; overflow-y: auto;
      display: flex; flex-direction: column; gap: 16px; scroll-behavior: smooth;
    }
    #messages::-webkit-scrollbar { width: 6px; }
    #messages::-webkit-scrollbar-track { background: transparent; }
    #messages::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.1); border-radius: 10px; }
    
    @keyframes slideUpFade {
      from { opacity: 0; transform: translateY(15px); }
      to { opacity: 1; transform: translateY(0); }
    }
    
    .message {
      width: fit-content; max-width: min(85%, 600px);
      display: grid; gap: 6px; align-self: flex-start;
      animation: slideUpFade 0.3s cubic-bezier(0.16, 1, 0.3, 1) forwards;
    }
    .message.own { align-self: flex-end; }
    
    .meta { color: var(--muted); font-size: 11px; padding: 0 6px; font-weight: 500; letter-spacing: 0.3px; }
    .message.own .meta { text-align: right; }
    
    .bubble {
      padding: 14px 18px; border-radius: 18px; border-bottom-left-radius: 4px;
      background: var(--bubble-other); backdrop-filter: blur(10px);
      color: var(--text); line-height: 1.5; overflow-wrap: anywhere;
      border: 1px solid var(--glass-border); font-size: 15px;
    }
    .own .bubble {
      background: var(--bubble-own); border: none;
      border-bottom-left-radius: 18px; border-bottom-right-radius: 4px;
      box-shadow: 0 6px 20px rgba(121, 40, 202, 0.25);
    }
    
    .system {
      align-self: center; max-width: 90%; color: var(--muted); font-size: 12px;
      text-align: center; padding: 8px 16px; border-radius: 999px;
      background: var(--system-bg); border: 1px solid var(--glass-border);
      animation: slideUpFade 0.4s ease forwards;
    }
    
    #composer {
      display: flex; gap: 12px; padding: 20px 24px;
      background: rgba(0, 0, 0, 0.2); border-top: 1px solid var(--glass-border);
    }
    input {
      flex: 1; min-width: 0; border: 1px solid var(--glass-border); border-radius: 12px;
      background: rgba(255, 255, 255, 0.03); color: #fff; padding: 14px 18px;
      font-size: 15px; outline: none; transition: all 0.2s ease;
    }
    input:focus {
      background: rgba(255, 255, 255, 0.06); border-color: var(--accent3); 
      box-shadow: 0 0 0 4px rgba(0, 212, 255, 0.1);
    }
    button {
      border: 0; border-radius: 12px;
      background: linear-gradient(135deg, var(--accent3), #0072ff);
      color: #fff; min-height: 48px; padding: 0 24px;
      font-weight: 600; font-size: 15px; cursor: pointer;
      transition: all 0.2s ease; box-shadow: 0 4px 15px rgba(0, 212, 255, 0.2);
    }
    button:hover:not(:disabled) {
      transform: translateY(-2px); box-shadow: 0 6px 20px rgba(0, 212, 255, 0.3);
    }
    button:disabled { opacity: 0.5; cursor: not-allowed; transform: none; box-shadow: none; }
    
    main:not(.joined) #messages, main:not(.joined) #composer { display: none; }
    main.joined #joinPanel { display: none; }
    
    @media (max-width: 640px) {
      body { padding: 0; }
      main { width: 100%; height: 100vh; border: 0; border-radius: 0; }
      header { padding: 16px; }
      .message { max-width: 90%; }
      #messages, #composer { padding: 16px; }
      .join-box { border: none; background: transparent; box-shadow: none; padding: 20px; }
    }
  </style>
</head>
<body>
  <main id="app">
    <header>
      <div class="brand">
        <div class="mark">FC</div>
        <h1>Family Chat</h1>
      </div>
      <div id="status">Waiting to join</div>
    </header>
    <section id="joinPanel">
      <form id="joinForm" class="join-box">
        <h2>Join the room</h2>
        <div class="join-row">
          <input id="name" autocomplete="name" placeholder="Your name" maxlength="32">
          <button type="submit">Join</button>
        </div>
      </form>
    </section>
    <section id="messages" aria-live="polite"></section>
    <form id="composer">
      <input id="message" autocomplete="off" placeholder="Type a message" maxlength="500">
      <button id="send" type="submit" disabled>Send</button>
    </form>
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
      meta.textContent = `${sender} - ${now()}`;

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

// -----------------------------------------------------------------------------
//  Per-client worker thread
// -----------------------------------------------------------------------------
//
// Reads newline-delimited lines from a single client, treating the first line
// as the display name and broadcasting the rest. Cleans up on disconnect.
void handle_client(int client_fd, std::string peer_address) {
    char   buffer[chat::RECV_BUFFER_SIZE];
    std::string inbox;          // Accumulates bytes until a full line arrives.
    std::string display_name;   // Empty until the client sends its name.

    while (g_running.load()) {
        ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            // 0  => client closed the connection cleanly.
            // <0 => socket error.
            break;
        }

        inbox.append(buffer, static_cast<size_t>(received));

        if (display_name.empty() &&
            (starts_with(inbox, "GET ") || starts_with(inbox, "POST ") ||
             starts_with(inbox, "OPTIONS "))) {
            handle_http_client(client_fd, inbox, peer_address);
            return;
        }

        // Pull every complete line out of the inbox buffer.
        size_t newline_pos;
        while ((newline_pos = inbox.find(chat::MESSAGE_DELIMITER)) != std::string::npos) {
            std::string line = inbox.substr(0, newline_pos);
            inbox.erase(0, newline_pos + 1);

            // Strip a trailing carriage return (handles CRLF clients).
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (display_name.empty()) {
                // First line = the client's chosen name.
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
                std::cout << formatted;          // Echo to server console.
                broadcast(formatted, client_fd); // Relay to everyone else.
            }
        }
    }

    // ---- Cleanup on disconnect ----
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
    // Port selection priority:
    //   1. Command-line argument (e.g. ./server 6000)
    //   2. The PORT environment variable (injected by Railway and most PaaS)
    //   3. The compiled-in default (chat::DEFAULT_PORT)
    uint16_t port = chat::DEFAULT_PORT;
    if (const char* env_port = std::getenv("PORT")) {
        if (*env_port) {
            port = static_cast<uint16_t>(std::stoi(env_port));
        }
    }
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    // ---- Create the listening socket ----
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("socket");
        return 1;
    }

    // Allow quick restart of the server without "address already in use".
    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;   // Listen on all interfaces.
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

    // ---- Acceptor loop ----
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

        // Build a "ip:port" string for logging.
        char ip_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::string peer = std::string(ip_str) + ":" +
                           std::to_string(ntohs(client_addr.sin_port));

        // Hand the client off to a detached worker thread.
        std::thread(handle_client, client_fd, peer).detach();
    }

    ::close(listen_fd);
    return 0;
}
