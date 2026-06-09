// =============================================================================
//  Chat Client
// =============================================================================
//
//  A TCP chat client written in C++ using POSIX sockets.
//
//  Architecture
//  ------------
//   * The main thread reads lines from stdin and sends them to the server.
//   * A background "receiver" thread continuously reads messages from the
//     server and prints them, so incoming chat appears in real time even
//     while the user is idle at the prompt.
//
//  Protocol
//  --------
//   The first line sent after connecting is the user's display name. Every
//   subsequent line is a chat message. Type "/quit" to disconnect.
//
//  Build
//  -----
//   g++ -std=c++17 -pthread client.cpp -o client
//
//  Run
//  ---
//   ./client [host] [port]
//
//   `host` may be an IP address (127.0.0.1) OR a domain name
//   (e.g. your-app.up.railway.app), since connections are resolved via DNS.
//
// =============================================================================

#include "common.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Set to false when either the user quits or the server closes the link.
std::atomic<bool> g_connected{true};

// Writes the entire contents of `data` to `fd`, retrying on partial writes.
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

// Background thread: continuously reads from the server and prints each
// complete, newline-delimited message to stdout.
void receive_loop(int sock_fd) {
    char        buffer[chat::RECV_BUFFER_SIZE];
    std::string inbox;

    while (g_connected.load()) {
        ssize_t received = ::recv(sock_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            // Server closed the connection or an error occurred.
            std::cout << "\n[client] Disconnected from server.\n";
            g_connected.store(false);
            break;
        }

        inbox.append(buffer, static_cast<size_t>(received));

        size_t newline_pos;
        while ((newline_pos = inbox.find(chat::MESSAGE_DELIMITER)) != std::string::npos) {
            std::string line = inbox.substr(0, newline_pos);
            inbox.erase(0, newline_pos + 1);
            std::cout << line << "\n";
            std::cout.flush();
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string host = chat::DEFAULT_HOST;
    uint16_t    port = chat::DEFAULT_PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));

    // ---- Resolve the host (works for both IPs and domain names) ----
    // Railway gives you a public domain such as "myapp.up.railway.app",
    // so we use getaddrinfo() instead of inet_pton() to support DNS lookups.
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;    // Allow IPv4 or IPv6.
    hints.ai_socktype = SOCK_STREAM;  // TCP.

    addrinfo* results = nullptr;
    std::string port_str = std::to_string(port);
    int gai = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &results);
    if (gai != 0) {
        std::cerr << "[client] Could not resolve " << host << ": "
                  << ::gai_strerror(gai) << "\n";
        return 1;
    }

    // ---- Try each resolved address until one connects ----
    int sock_fd = -1;
    for (addrinfo* rp = results; rp != nullptr; rp = rp->ai_next) {
        sock_fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) {
            continue;
        }
        if (::connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  // Success.
        }
        ::close(sock_fd);
        sock_fd = -1;
    }

    ::freeaddrinfo(results);

    if (sock_fd < 0) {
        std::cerr << "[client] Unable to connect to " << host << ":" << port
                  << "\n";
        return 1;
    }

    std::cout << "[client] Connected to " << host << ":" << port << "\n";

    // ---- Ask for a display name and send it as the first line ----
    std::string name;
    std::cout << "Enter your name: ";
    std::getline(std::cin, name);
    if (name.empty()) {
        name = "anonymous";
    }
    send_all(sock_fd, name + "\n");

    std::cout << "[client] Type messages and press Enter. Type \"/quit\" to exit.\n";

    // ---- Start the background receiver thread ----
    std::thread receiver(receive_loop, sock_fd);

    // ---- Main loop: read stdin and send to the server ----
    std::string line;
    while (g_connected.load() && std::getline(std::cin, line)) {
        if (line == "/quit") {
            break;
        }
        if (!g_connected.load()) {
            break;
        }
        if (!send_all(sock_fd, line + "\n")) {
            std::cout << "[client] Failed to send. Connection lost.\n";
            break;
        }
    }

    // ---- Shutdown ----
    g_connected.store(false);
    ::shutdown(sock_fd, SHUT_RDWR);  // Unblocks the receiver's recv().
    ::close(sock_fd);

    if (receiver.joinable()) {
        receiver.join();
    }

    std::cout << "[client] Goodbye!\n";
    return 0;
}
