#ifndef CHAT_COMMON_H
#define CHAT_COMMON_H

// Shared definitions used by both the chat server and the chat client.
//
// The protocol is intentionally simple: messages are plain UTF-8 text
// terminated by a newline character ('\n'). This makes the wire format
// human-readable and easy to debug with tools like `telnet` or `nc`.

#include <string>
#include <cstdint>

namespace chat {

// Default network configuration. These can be overridden from the command
// line when launching the server or the client.
constexpr const char* DEFAULT_HOST = "127.0.0.1";
constexpr uint16_t     DEFAULT_PORT = 5555;

// Maximum size (in bytes) of a single read from a socket. Long messages are
// reassembled across multiple reads by the line-buffering logic.
constexpr size_t RECV_BUFFER_SIZE = 4096;

// Maximum number of clients the server will keep in its backlog queue while
// waiting to accept() new connections.
constexpr int LISTEN_BACKLOG = 16;

// The character that delimits a complete protocol message on the wire.
constexpr char MESSAGE_DELIMITER = '\n';

} // namespace chat

#endif // CHAT_COMMON_H
