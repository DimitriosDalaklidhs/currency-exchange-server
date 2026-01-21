# Currency Exchange Server & Client

A C-based TCP client-server application that simulates a multi-user currency exchange system with persistent accounts, balances, and currency conversion.  
Developed, refined and published as a personal **repository**.

---

## Overview

This project demonstrates low-level network programming in C using the **Berkeley sockets API**.  
It includes both a server and a client:

- **Server**  
  - Listens on TCP port `8080`  
  - Spawns a child process (`fork()`) for each incoming connection  
  - Implements a simple text-based application protocol  
  - Manages users, accounts, and balances  
  - Supports multiple currencies and fixed exchange rates  
  - Uses file-backed persistence with POSIX file locking  
  - Closes connections gracefully  

- **Client**  
  - Connects to the server using an IP passed as a command-line argument  
  - Exchanges commands and responses through sockets  
  - Displays server output deterministically using an `END`-terminated protocol  

---

## Key Concepts Demonstrated

- TCP socket creation and communication  
- Process forking for handling multiple clients  
- Application-level protocol design  
- File-backed persistence and POSIX file locking  
- String parsing and command validation  
- Defensive systems programming in C  
- Graceful handling of socket and I/O errors  

---

## Compile

```bash
gcc -o server server.c
gcc -o client client.c
```

Or with the provided Makefile:

make

**Run Client (in another terminal)**
```bash
./client 127.0.0.1
```

(Replace 127.0.0.1 with the server IP if running on another machine or inside a VM.)

**Application Protocol**:
All server responses end with:
```powershell
END
```

This ensures deterministic client-side parsing and avoids buffering issues.

Supported Commands
```php-template
REGISTER <user> <pass>
LOGIN <user> <pass>
RATES
CREATE_ACCOUNT IND|JOINT <ownersCSV>
LIST_ACCOUNTS
BALANCES <accid>
DEPOSIT <accid> <CUR> <amount>
WITHDRAW <accid> <CUR> <amount>
EXCHANGE <accid> <FROMCUR> <TOCUR> <amount>
QUIT
```

Supported currencies: 
- USD 
- EUR 
- GBP.

**Data Persistence**

All users and accounts are stored in:
```powershell
exchange_db.txt
```
The database file is:

- Automatically created if missing
- Safely shared across multiple clients
- Protected using advisory file locks (fcntl)

Project Structure
```bash
currency-exchange-server/
│
├── client.c    # TCP client implementation
├── server.c    # TCP server implementation
├── .gitignore
├── LICENSE
└── README.md
```
***DOCKER***

**Build**
```bash
docker build -t currency-exchange-server .
```

Run (ephemeral):
```bash
docker run --rm -p 8080:8080 currency-exchange-server
```
Run with persistent database:

The server writes exchange_db.txt to the container working directory. The Docker image sets the working directory to /data,
so mount a volume there to persist the database:
```bash
docker run --rm -p 8080:8080 \
  -v "$(pwd)/data:/data" \
  currency-exchange-server
```
This will create/use:

- ./data/exchange_db.txt on the host (persisted across runs)

**Connect from the client**

If you’re running the client on the host machine:
-
./client 127.0.0.1


If the client is also containerized later, you can run both on the same Docker network (example in Compose below).

 Security & runtime notes (Docker)

- Multi-stage build: compiles in gcc:13, runs on debian:bookworm-slim for a smaller attack surface.

- Basic hardening flags enabled (-fstack-protector-strong, -D_FORTIFY_SOURCE=2, PIE, RELRO/NOW).

- Runs as a non-root user (appuser).

- Uses /data as a writable directory (avoids writing into the image filesystem).

- Healthcheck uses nc to verify port 8080 is accepting connections.
  
**Optional: Docker Compose**
-
Create compose.yml:
```yaml
services:
  server:
    build: .
    ports:
      - "8080:8080"
    volumes:
      - ./data:/data
```
**Recommended .dockerignore**

This keeps builds fast and clean:
```dockerignore
.git
.gitignore
README.md
data
*.o
client
server
```
