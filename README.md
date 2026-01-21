# Currency Exchange Server & Client

A C-based TCP client-server application that simulates a multi-user currency exchange system with persistent accounts, balances, and currency conversion.  
Originally developed as coursework and later refined and published as a personal **repository**.

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

