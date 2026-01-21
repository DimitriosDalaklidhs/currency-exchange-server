/* server.c - Currency Exchange Server (TCP, fork per client, file-locking DB)
 * Protocol: EVERY server response ends with "END\n"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

#define PORT 8080
#define BUFFER_SIZE 512

#define DB_FILE "exchange_db.txt"

#define MAX_USERS 200
#define MAX_ACCOUNTS 500
#define MAX_OWNERS 5

#define USERNAME_LEN 32
#define PASS_LEN 32
#define ACCID_LEN 32
#define INT_LEN 12

typedef enum { CUR_USD = 0, CUR_EUR = 1, CUR_GBP = 2, CUR_COUNT = 3 } Currency;

static const char *CUR_NAMES[CUR_COUNT] = { "USD", "EUR", "GBP" };

/* Rates: 1 unit of FROM -> ? units of TO */
static double rate(Currency from, Currency to) {
    /* base values relative to EUR (approx example): 1 EUR = 1.10 USD, 1 EUR = 0.85 GBP */
    double eur_to_usd = 1.10;
    double eur_to_gbp = 0.85;

    double val_in_eur;

    if (from == to) return 1.0;

    /* Convert FROM to EUR */
    if (from == CUR_EUR) val_in_eur = 1.0;
    else if (from == CUR_USD) val_in_eur = 1.0 / eur_to_usd;
    else /* GBP */ val_in_eur = 1.0 / eur_to_gbp;

    /* Convert EUR to TO */
    if (to == CUR_EUR) return val_in_eur;
    else if (to == CUR_USD) return val_in_eur * eur_to_usd;
    else /* GBP */ return val_in_eur * eur_to_gbp;
}

typedef struct {
    char username[USERNAME_LEN];
    char password[PASS_LEN];
} User;

typedef struct {
    char id[ACCID_LEN];
    int isJoint; /* 0 individual, 1 joint */
    int ownerCount;
    char owners[MAX_OWNERS][USERNAME_LEN];
    double bal[CUR_COUNT];
} Account;

typedef struct {
    User users[MAX_USERS];
    int userCount;

    Account accounts[MAX_ACCOUNTS];
    int accCount;
} DB;

void errMsg(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* --------- file locking (fcntl) ---------- */
static void lock_file(int fd, short l_type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = l_type;      /* F_RDLCK / F_WRLCK */
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;            /* whole file */
    while (fcntl(fd, F_SETLKW, &fl) == -1) {
        if (errno == EINTR) continue;
        errMsg("fcntl lock");
    }
}

static void unlock_file(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(fd, F_SETLK, &fl) == -1)
        errMsg("fcntl unlock");
}

/* --------- helpers ---------- */
static int parse_currency(const char *s) {
    for (int i = 0; i < CUR_COUNT; i++) {
        if (strcmp(s, CUR_NAMES[i]) == 0) return i;
    }
    return -1;
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

static int user_index(DB *db, const char *username) {
    for (int i = 0; i < db->userCount; i++) {
        if (strcmp(db->users[i].username, username) == 0) return i;
    }
    return -1;
}

static int account_index(DB *db, const char *accid) {
    for (int i = 0; i < db->accCount; i++) {
        if (strcmp(db->accounts[i].id, accid) == 0) return i;
    }
    return -1;
}

static int is_owner(Account *a, const char *username) {
    for (int i = 0; i < a->ownerCount; i++) {
        if (strcmp(a->owners[i], username) == 0) return 1;
    }
    return 0;
}

static int gen_account_id(DB *db, char *out, size_t outsz) {
    /* simple ID: ACC<number> ; ensure uniqueness */
    for (int tries = 0; tries < 10000; tries++) {
        int n = 1000 + (rand() % 9000);
        snprintf(out, outsz, "ACC%d", n);
        if (account_index(db, out) == -1) return 0;
    }
    return -1;
}

/* --------- DB load/save ---------- */
static void db_init(DB *db) {
    memset(db, 0, sizeof(*db));
}

static void db_load_locked(int fd, DB *db) {
    /* fd is locked already (read or write) */
    db_init(db);

    lseek(fd, 0, SEEK_SET);
    FILE *fp = fdopen(dup(fd), "r");
    if (!fp) errMsg("fdopen");

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (line[0] == '\0') continue;

        if (strncmp(line, "USER ", 5) == 0) {
            if (db->userCount >= MAX_USERS) continue;

            char u[USERNAME_LEN], p[PASS_LEN];
            if (sscanf(line, "USER %31s %31s", u, p) == 2) {
                strncpy(db->users[db->userCount].username, u, USERNAME_LEN);
                strncpy(db->users[db->userCount].password, p, PASS_LEN);
                db->userCount++;
            }
        } else if (strncmp(line, "ACC ", 4) == 0) {
            if (db->accCount >= MAX_ACCOUNTS) continue;

            /* format:
               ACC <id> <type> <ownerCount> <owner1,owner2,...> <balUSD> <balEUR> <balGBP>
             */
            char id[ACCID_LEN], type[16], ownersCSV[256];
            int ownerCount = 0;
            double b0, b1, b2;

            if (sscanf(line, "ACC %31s %15s %d %255s %lf %lf %lf",
                       id, type, &ownerCount, ownersCSV, &b0, &b1, &b2) == 7) {

                Account *a = &db->accounts[db->accCount];
                memset(a, 0, sizeof(*a));
                strncpy(a->id, id, ACCID_LEN);
                a->isJoint = (strcmp(type, "JOINT") == 0) ? 1 : 0;
                a->ownerCount = ownerCount;
                if (a->ownerCount > MAX_OWNERS) a->ownerCount = MAX_OWNERS;

                /* split ownersCSV by comma */
                int idx = 0;
                char tmp[256];
                strncpy(tmp, ownersCSV, sizeof(tmp));
                tmp[sizeof(tmp)-1] = '\0';

                char *tok = strtok(tmp, ",");
                while (tok && idx < a->ownerCount) {
                    strncpy(a->owners[idx], tok, USERNAME_LEN);
                    a->owners[idx][USERNAME_LEN-1] = '\0';
                    idx++;
                    tok = strtok(NULL, ",");
                }
                a->ownerCount = idx;

                a->bal[CUR_USD] = b0;
                a->bal[CUR_EUR] = b1;
                a->bal[CUR_GBP] = b2;

                db->accCount++;
            }
        }
    }

    fclose(fp);
}

static void db_save_locked(int fd, DB *db) {
    /* fd is locked for write already */
    if (ftruncate(fd, 0) == -1) errMsg("ftruncate");
    lseek(fd, 0, SEEK_SET);

    /* write USERS */
    for (int i = 0; i < db->userCount; i++) {
        dprintf(fd, "USER %s %s\n", db->users[i].username, db->users[i].password);
    }

    /* write ACCOUNTS */
    for (int i = 0; i < db->accCount; i++) {
        Account *a = &db->accounts[i];
        char ownersCSV[256] = {0};
        for (int k = 0; k < a->ownerCount; k++) {
            strcat(ownersCSV, a->owners[k]);
            if (k < a->ownerCount - 1) strcat(ownersCSV, ",");
        }
        dprintf(fd, "ACC %s %s %d %s %.2f %.2f %.2f\n",
                a->id,
                a->isJoint ? "JOINT" : "IND",
                a->ownerCount,
                ownersCSV[0] ? ownersCSV : "-",
                a->bal[CUR_USD], a->bal[CUR_EUR], a->bal[CUR_GBP]);
    }

    fsync(fd);
}

/* --------- protocol helpers ---------- */
static int send_all(int fd, const char *s) {
    size_t len = strlen(s);
    ssize_t w = write(fd, s, len);
    return (w == (ssize_t)len) ? 0 : -1;
}

static int recv_line(int fd, char *buf, size_t bufsz) {
    size_t i = 0;
    while (i + 1 < bufsz) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) return 0;          /* closed */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return 1;
}

/* --------- commands ---------- */
static void cmd_help(int cfd) {
    send_all(cfd,
        "OK Commands:\n"
        "  REGISTER <user> <pass>\n"
        "  LOGIN <user> <pass>\n"
        "  RATES\n"
        "  CREATE_ACCOUNT IND|JOINT <ownersCSV>\n"
        "  LIST_ACCOUNTS\n"
        "  BALANCES <accid>\n"
        "  DEPOSIT <accid> <CUR> <amount>\n"
        "  WITHDRAW <accid> <CUR> <amount>\n"
        "  EXCHANGE <accid> <FROMCUR> <TOCUR> <amount>\n"
        "  QUIT\n"
        "END\n");
}

static void cmd_rates(int cfd) {
    char out[512];
    snprintf(out, sizeof(out),
             "OK Rates (approx, fixed):\n"
             "  1 EUR = %.2f USD\n"
             "  1 EUR = %.2f GBP\n"
             "END\n",
             rate(CUR_EUR, CUR_USD),
             rate(CUR_EUR, CUR_GBP));
    send_all(cfd, out);
}

static void cmd_register(int cfd, int dbfd, const char *u, const char *p) {
    lock_file(dbfd, F_WRLCK);

    DB db;
    db_load_locked(dbfd, &db);

    if (user_index(&db, u) != -1) {
        unlock_file(dbfd);
        send_all(cfd, "ERR User already exists\nEND\n");
        return;
    }

    if (db.userCount >= MAX_USERS) {
        unlock_file(dbfd);
        send_all(cfd, "ERR User limit reached\nEND\n");
        return;
    }

    strncpy(db.users[db.userCount].username, u, USERNAME_LEN);
    strncpy(db.users[db.userCount].password, p, PASS_LEN);
    db.userCount++;

    db_save_locked(dbfd, &db);
    unlock_file(dbfd);

    send_all(cfd, "OK Registered\nEND\n");
}

static int cmd_login(int cfd, int dbfd, const char *u, const char *p, char *loggedUser) {
    lock_file(dbfd, F_RDLCK);

    DB db;
    db_load_locked(dbfd, &db);

    int idx = user_index(&db, u);
    if (idx == -1) {
        unlock_file(dbfd);
        send_all(cfd, "ERR No such user\nEND\n");
        return 0;
    }
    if (strcmp(db.users[idx].password, p) != 0) {
        unlock_file(dbfd);
        send_all(cfd, "ERR Wrong password\nEND\n");
        return 0;
    }

    unlock_file(dbfd);
    strncpy(loggedUser, u, USERNAME_LEN);
    send_all(cfd, "OK Logged in\nEND\n");
    return 1;
}

static void cmd_create_account(int cfd, int dbfd, const char *loggedUser,
                               const char *type, const char *ownersCSV) {
    if (loggedUser[0] == '\0') {
        send_all(cfd, "ERR Please LOGIN first\nEND\n");
        return;
    }

    int isJoint = 0;
    if (strcmp(type, "IND") == 0) isJoint = 0;
    else if (strcmp(type, "JOINT") == 0) isJoint = 1;
    else {
        send_all(cfd, "ERR type must be IND or JOINT\nEND\n");
        return;
    }

    lock_file(dbfd, F_WRLCK);

    DB db;
    db_load_locked(dbfd, &db);

    if (db.accCount >= MAX_ACCOUNTS) {
        unlock_file(dbfd);
        send_all(cfd, "ERR Account limit reached\nEND\n");
        return;
    }

    Account a;
    memset(&a, 0, sizeof(a));
    a.isJoint = isJoint;

    if (gen_account_id(&db, a.id, sizeof(a.id)) == -1) {
        unlock_file(dbfd);
        send_all(cfd, "ERR Could not generate account id\nEND\n");
        return;
    }

    /* parse owners */
    char tmp[256];
    strncpy(tmp, ownersCSV, sizeof(tmp));
    tmp[sizeof(tmp)-1] = '\0';

    int ownerCount = 0;
    char *tok = strtok(tmp, ",");
    while (tok && ownerCount < MAX_OWNERS) {
        if (user_index(&db, tok) == -1) {
            unlock_file(dbfd);
            send_all(cfd, "ERR One or more owners do not exist (REGISTER them first)\nEND\n");
            return;
        }
        strncpy(a.owners[ownerCount], tok, USERNAME_LEN);
        a.owners[ownerCount][USERNAME_LEN-1] = '\0';
        ownerCount++;
        tok = strtok(NULL, ",");
    }

    if (ownerCount == 0) {
        unlock_file(dbfd);
        send_all(cfd, "ERR ownersCSV is empty\nEND\n");
        return;
    }

    /* For IND, enforce only one owner and must be loggedUser */
    if (!isJoint) {
        if (ownerCount != 1) {
            unlock_file(dbfd);
            send_all(cfd, "ERR IND account must have exactly 1 owner\nEND\n");
            return;
        }
        if (strcmp(a.owners[0], loggedUser) != 0) {
            unlock_file(dbfd);
            send_all(cfd, "ERR IND account owner must be the logged-in user\nEND\n");
            return;
        }
    } else {
        /* JOINT: require loggedUser included */
        int ok = 0;
        for (int i = 0; i < ownerCount; i++) {
            if (strcmp(a.owners[i], loggedUser) == 0) ok = 1;
        }
        if (!ok) {
            unlock_file(dbfd);
            send_all(cfd, "ERR JOINT account must include logged-in user among owners\nEND\n");
            return;
        }
    }

    a.ownerCount = ownerCount;
    for (int i = 0; i < CUR_COUNT; i++) a.bal[i] = 0.0;

    db.accounts[db.accCount++] = a;

    db_save_locked(dbfd, &db);
    unlock_file(dbfd);

    char out[128];
    snprintf(out, sizeof(out), "OK Created %s\nEND\n", a.id);
    send_all(cfd, out);
}

static void cmd_list_accounts(int cfd, int dbfd, const char *loggedUser) {
    if (loggedUser[0] == '\0') {
        send_all(cfd, "ERR Please LOGIN first\nEND\n");
        return;
    }

    lock_file(dbfd, F_RDLCK);

    DB db;
    db_load_locked(dbfd, &db);

    send_all(cfd, "OK Accounts:\n");
    for (int i = 0; i < db.accCount; i++) {
        Account *a = &db.accounts[i];
        if (!is_owner(a, loggedUser)) continue;

        char ownersCSV[256] = {0};
        for (int k = 0; k < a->ownerCount; k++) {
            strcat(ownersCSV, a->owners[k]);
            if (k < a->ownerCount - 1) strcat(ownersCSV, ",");
        }

        char line[512];
        snprintf(line, sizeof(line),
                 "  %s  %s  owners=%s\n",
                 a->id, a->isJoint ? "JOINT" : "IND", ownersCSV);
        send_all(cfd, line);
    }
    send_all(cfd, "END\n");

    unlock_file(dbfd);
}

static void cmd_balances(int cfd, int dbfd, const char *loggedUser, const char *accid) {
    if (loggedUser[0] == '\0') {
        send_all(cfd, "ERR Please LOGIN first\nEND\n");
        return;
    }

    lock_file(dbfd, F_RDLCK);

    DB db;
    db_load_locked(dbfd, &db);

    int idx = account_index(&db, accid);
    if (idx == -1) {
        unlock_file(dbfd);
        send_all(cfd, "ERR No such account\nEND\n");
        return;
    }

    Account *a = &db.accounts[idx];
    if (!is_owner(a, loggedUser)) {
        unlock_file(dbfd);
        send_all(cfd, "ERR Not an owner\nEND\n");
        return;
    }

    char out[256];
    snprintf(out, sizeof(out),
             "OK %s balances: USD=%.2f EUR=%.2f GBP=%.2f\nEND\n",
             a->id, a->bal[CUR_USD], a->bal[CUR_EUR], a->bal[CUR_GBP]);
    unlock_file(dbfd);
    send_all(cfd, out);
}

static void cmd_deposit_withdraw(int cfd, int dbfd, const char *loggedUser,
                                 const char *op, const char *accid, const char *curS, double amount) {
    if (loggedUser[0] == '\0') {
        send_all(cfd, "ERR Please LOGIN first\nEND\n");
        return;
    }
    if (amount <= 0.0) {
        send_all(cfd, "ERR amount must be > 0\nEND\n");
        return;
    }

    int cur = parse_currency(curS);
    if (cur < 0) {
        send_all(cfd, "ERR Unknown currency (USD/EUR/GBP)\nEND\n");
        return;
    }

    lock_file(dbfd, F_WRLCK); /* critical section */

    DB db;
    db_load_locked(dbfd, &db);

    int idx = account_index(&db, accid);
    if (idx == -1) {
        unlock_file(dbfd);
        send_all(cfd, "ERR No such account\nEND\n");
        return;
    }

    Account *a = &db.accounts[idx];
    if (!is_owner(a, loggedUser)) {
        unlock_file(dbfd);
        send_all(cfd, "ERR Not an owner\nEND\n");
        return;
    }

    if (strcmp(op, "DEPOSIT") == 0) {
        a->bal[cur] += amount;
    } else if (strcmp(op, "WITHDRAW") == 0) {
        if (a->bal[cur] < amount) {
            unlock_file(dbfd);
            send_all(cfd, "ERR Insufficient funds\nEND\n");
            return;
        }
        a->bal[cur] -= amount;
    } else {
        unlock_file(dbfd);
        send_all(cfd, "ERR Internal op\nEND\n");
        return;
    }

    db_save_locked(dbfd, &db);
    unlock_file(dbfd);

    send_all(cfd, "OK Done\nEND\n");
}

static void cmd_exchange(int cfd, int dbfd, const char *loggedUser,
                         const char *accid, const char *fromS, const char *toS, double amount) {
    if (loggedUser[0] == '\0') {
        send_all(cfd, "ERR Please LOGIN first\nEND\n");
        return;
    }
    if (amount <= 0.0) {
        send_all(cfd, "ERR amount must be > 0\nEND\n");
        return;
    }

    int from = parse_currency(fromS);
    int to = parse_currency(toS);
    if (from < 0 || to < 0) {
        send_all(cfd, "ERR Unknown currency (USD/EUR/GBP)\nEND\n");
        return;
    }
    if (from == to) {
        send_all(cfd, "ERR FROMCUR and TOCUR must differ\nEND\n");
        return;
    }

    lock_file(dbfd, F_WRLCK); /* critical section */

    DB db;
    db_load_locked(dbfd, &db);

    int idx = account_index(&db, accid);
    if (idx == -1) {
        unlock_file(dbfd);
        send_all(cfd, "ERR No such account\nEND\n");
        return;
    }

    Account *a = &db.accounts[idx];
    if (!is_owner(a, loggedUser)) {
        unlock_file(dbfd);
        send_all(cfd, "ERR Not an owner\nEND\n");
        return;
    }

    if (a->bal[from] < amount) {
        unlock_file(dbfd);
        send_all(cfd, "ERR Insufficient funds\nEND\n");
        return;
    }

    double r = rate((Currency)from, (Currency)to);
    double converted = amount * r;

    a->bal[from] -= amount;
    a->bal[to] += converted;

    db_save_locked(dbfd, &db);
    unlock_file(dbfd);

    char out[256];
    snprintf(out, sizeof(out), "OK Exchanged %.2f %s -> %.2f %s (rate=%.6f)\nEND\n",
             amount, CUR_NAMES[from], converted, CUR_NAMES[to], r);
    send_all(cfd, out);
}

/* --------- client handler ---------- */
static void handleClient(int cfd, int dbfd) {
    char line[BUFFER_SIZE];
    char loggedUser[USERNAME_LEN] = {0};

    /* Welcome block */
    send_all(cfd, "OK Currency Exchange Server\nType HELP for commands\nEND\n");

    while (1) {
        if (send_all(cfd, "READY>\n") == -1) break;

        int rc = recv_line(cfd, line, sizeof(line));
        if (rc == 0) break;
        if (rc < 0) errMsg("read");

        trim_newline(line);
        if (line[0] == '\0') continue;

        char cmd[32];
        if (sscanf(line, "%31s", cmd) != 1) continue;

        if (strcmp(cmd, "HELP") == 0) {
            cmd_help(cfd);
        } else if (strcmp(cmd, "RATES") == 0) {
            cmd_rates(cfd);
        } else if (strcmp(cmd, "REGISTER") == 0) {
            char u[USERNAME_LEN], p[PASS_LEN];
            if (sscanf(line, "REGISTER %31s %31s", u, p) != 2) {
                send_all(cfd, "ERR Usage: REGISTER <user> <pass>\nEND\n");
            } else {
                cmd_register(cfd, dbfd, u, p);
            }
        } else if (strcmp(cmd, "LOGIN") == 0) {
            char u[USERNAME_LEN], p[PASS_LEN];
            if (sscanf(line, "LOGIN %31s %31s", u, p) != 2) {
                send_all(cfd, "ERR Usage: LOGIN <user> <pass>\nEND\n");
            } else {
                cmd_login(cfd, dbfd, u, p, loggedUser);
            }
        } else if (strcmp(cmd, "CREATE_ACCOUNT") == 0) {
            char type[16], ownersCSV[256];
            if (sscanf(line, "CREATE_ACCOUNT %15s %255s", type, ownersCSV) != 2) {
                send_all(cfd, "ERR Usage: CREATE_ACCOUNT IND|JOINT <ownersCSV>\nEND\n");
            } else {
                cmd_create_account(cfd, dbfd, loggedUser, type, ownersCSV);
            }
        } else if (strcmp(cmd, "LIST_ACCOUNTS") == 0) {
            cmd_list_accounts(cfd, dbfd, loggedUser);
        } else if (strcmp(cmd, "BALANCES") == 0) {
            char accid[ACCID_LEN];
            if (sscanf(line, "BALANCES %31s", accid) != 1) {
                send_all(cfd, "ERR Usage: BALANCES <accid>\nEND\n");
            } else {
                cmd_balances(cfd, dbfd, loggedUser, accid);
            }
        } else if (strcmp(cmd, "DEPOSIT") == 0 || strcmp(cmd, "WITHDRAW") == 0) {
            char accid[ACCID_LEN], curS[8];
            double amount;
            if (sscanf(line, "%31s %31s %7s %lf", cmd, accid, curS, &amount) != 4) {
                send_all(cfd, "ERR Usage: DEPOSIT|WITHDRAW <accid> <CUR> <amount>\nEND\n");
            } else {
                cmd_deposit_withdraw(cfd, dbfd, loggedUser, cmd, accid, curS, amount);
            }
        } else if (strcmp(cmd, "EXCHANGE") == 0) {
            char accid[ACCID_LEN], fromS[8], toS[8];
            double amount;
            if (sscanf(line, "EXCHANGE %31s %7s %7s %lf", accid, fromS, toS, &amount) != 4) {
                send_all(cfd, "ERR Usage: EXCHANGE <accid> <FROMCUR> <TOCUR> <amount>\nEND\n");
            } else {
                cmd_exchange(cfd, dbfd, loggedUser, accid, fromS, toS, amount);
            }
        } else if (strcmp(cmd, "QUIT") == 0) {
            send_all(cfd, "OK Bye\nEND\n");
            break;
        } else {
            send_all(cfd, "ERR Unknown command (try HELP)\nEND\n");
        }
    }

    close(cfd);
    exit(EXIT_SUCCESS);
}

int main() {
    int lfd, cfd;
    struct sockaddr_in serv_addr, client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int reuse = 1;

    /* open DB file once; children inherit fd */
    int dbfd = open(DB_FILE, O_RDWR | O_CREAT, 0644);
    if (dbfd == -1) errMsg("open DB_FILE");

    /* seed rand for account IDs */
    srand((unsigned) getpid());

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) errMsg("socket");

    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
        errMsg("setsockopt");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    if (bind(lfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        errMsg("bind");

    if (listen(lfd, 10) == -1)
        errMsg("listen");

    printf("Server listening on port %d\n", PORT);

    while (1) {
        cfd = accept(lfd, (struct sockaddr *)&client_addr, &addrlen);
        if (cfd == -1) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            close(cfd);
            continue;
        }

        if (pid == 0) {
            /* child */
            close(lfd);
            handleClient(cfd, dbfd);
        } else {
            /* parent */
            close(cfd);

            /* reap zombies (non-blocking) */
            while (waitpid(-1, NULL, WNOHANG) > 0) { }
        }
    }

    close(dbfd);
    close(lfd);
    return 0;
}
