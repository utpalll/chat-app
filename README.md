# C++ Chat Application (Client–Server with Sockets)

A multi-client chat application built in **C++17** using **POSIX TCP sockets**
and **threads**. It demonstrates a classic client–server network architecture:
a central server relays messages between any number of connected clients in
real time.

---

## Features

- **Client–server architecture** over TCP/IP.
- **Multiple simultaneous clients** — each connection is served on its own thread.
- **Real-time broadcast** — a message from one client is delivered to all others.
- **Join / leave notifications** broadcast to everyone.
- **Named users** — each client picks a display name on connect.
- **Clean disconnect** with the `/quit` command.
- **Robust I/O** — handles partial reads/writes and reassembles messages that
  span multiple TCP segments using newline-delimited framing.

---

## Project Structure

| File         | Purpose                                                        |
|--------------|----------------------------------------------------------------|
| `common.h`     | Shared constants and protocol definitions used by both sides.  |
| `server.cpp`   | The chat server: accepts connections and broadcasts messages.  |
| `client.cpp`   | The chat client: sends user input and prints incoming chat.    |
| `Makefile`     | Build script for both binaries.                                |
| `Dockerfile`   | Builds and runs the server in a container (used for deploying). |
| `railway.json` | Railway deployment configuration.                              |
| `.dockerignore`| Keeps local binaries out of the Docker build context.          |

---

## How It Works

### Protocol
Messages are **plain UTF-8 text terminated by a newline (`\n`)**. This keeps the
wire format human-readable (you can even test the server with `telnet` or `nc`).

1. When a client connects, the **first line** it sends is its **display name**.
2. Every **subsequent line** is a chat message, which the server prefixes with
   the sender's name and relays to all other clients.

### Server
- Creates a listening socket (`socket` → `bind` → `listen`).
- Runs an **acceptor loop** that `accept()`s new clients.
- Spawns a **detached worker thread** per client (`handle_client`).
- Maintains a **mutex-protected registry** of connected clients so it can
  broadcast safely from multiple threads.

### Client
- Connects to the server (`socket` → `connect`).
- Spawns a **background receiver thread** that prints incoming messages.
- Uses the **main thread** to read stdin and send messages, so you can receive
  and send at the same time.

---

## Build

Requires a C++17 compiler (e.g. `g++`) on Linux or macOS.

```bash
make
```

This produces two executables: `server` and `client`.

> You can also build manually:
> ```bash
> g++ -std=c++17 -pthread server.cpp -o server
> g++ -std=c++17 -pthread client.cpp -o client
> ```

---

## Run

Open **separate terminals**.

**1. Start the server** (defaults to port `5555`):
```bash
./server
# or choose a port:
./server 6000
```

**2. Start one or more clients:**
```bash
./client
# or connect to a specific host/port:
./client 127.0.0.1 6000
```

Each client will prompt for a name, then you can start chatting. Type `/quit`
to leave.

---

## Example Session

```
# Terminal 1 (server)
[server] Chat server listening on port 5555
[server] alice joined from 127.0.0.1:51000 (1 online)
[server] bob joined from 127.0.0.1:51001 (2 online)
alice: hello!
bob: hey alice

# Terminal 2 (alice)
Enter your name: alice
*** Welcome, alice! You are now connected. ***
*** bob has joined the chat ***
bob: hey alice

# Terminal 3 (bob)
Enter your name: bob
*** Welcome, bob! You are now connected. ***
alice: hello!
```

---

## Clean Up

```bash
make clean
```

---

## Deploy the Server Online (Railway)

Railway runs your server in a long-lived Docker container and gives it a public
address — exactly what a persistent TCP socket server needs. (Serverless hosts
like Vercel **cannot** run a raw TCP server, which is why we use Railway.)

### Steps

1. **Push this project to a GitHub repository.**

2. **Create a Railway project:**
   - Go to [railway.app](https://railway.app) and sign in.
   - Click **New Project → Deploy from GitHub repo** and pick this repo.
   - Railway detects the `Dockerfile` and builds the server automatically.

3. **Expose a public TCP port:**
   - Open your service → **Settings → Networking**.
   - Under **TCP Proxy**, click **Generate TCP Proxy** (or "Add TCP Proxy").
   - Railway gives you a public host and port, e.g.
     `containers-us-west-1.railway.app : 7531`.
   - Railway also injects a `PORT` environment variable into the container,
     and the server reads it automatically — no code change needed.

4. **Connect from anywhere** using your local client with the Railway host/port:
   ```bash
   ./client containers-us-west-1.railway.app 7531
   ```
   The client resolves domain names via DNS, so the Railway hostname works
   directly.

> **Note on the TCP proxy port:** the public proxy port (e.g. `7531`) is
> different from the internal `PORT` the server listens on inside the
> container. Railway maps one to the other for you — just always connect using
> the **public** host and port shown in the Networking settings.

### Deploy with the Railway CLI (alternative)

```bash
npm i -g @railway/cli
railway login
railway init        # create / link a project
railway up          # build & deploy using the Dockerfile
```

Then add a TCP proxy from the dashboard as described above.

---

## Notes & Possible Extensions

- The server listens on all interfaces (`INADDR_ANY`), so clients on the same
  network can connect using the server machine's IP address.
- Ideas to extend this for a fuller project:
  - Private/direct messages (`/msg <user> ...`).
  - A `/who` command to list online users.
  - Chat rooms / channels.
  - TLS encryption with OpenSSL.
  - Windows support via Winsock (`#ifdef _WIN32`).
