#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct serial_conf {
    char *port;
    char *msg;
    char *term;
    int do_ping;
    int do_transaction;         /* OP 4 */
    int transaction_timeout_ms; /**/
    int do_manual_ctrl;         /* OP 1.4 */
    int set_dtr;                /* OP 1.4: -1=ignore, 0=clear, 1=set */
    int set_rts;                /* OP 1.4: -1=ignore, 0=clear, 1=set */
    int binary_mode;            /* OP 6.2 */
    int do_listen;              /* only RX, no TX */
    speed_t baud;               /**/
    int data_bits;              /* 7 or 8 */
    char parity;                /* N, E, O */
    int stop_bits;              /* 1 or 2 */
    int flow_ctrl;              /* 0:none 1:RTS/CTS 2:XON/XOFF 3:DTR/DSR */
};


volatile int keep_running = 1;
void handle_sigint(int sig) { keep_running = 0; }


typedef struct {
    int val;
    speed_t spd;
} baud_entry_t;

static const baud_entry_t baud_table[] = {{150, B150},     {300, B300},     {600, B600},      {1200, B1200},
                                          {2400, B2400},   {4800, B4800},   {9600, B9600},    {19200, B19200},
                                          {38400, B38400}, {57600, B57600}, {115200, B115200}};


speed_t baud_from_int(const int b) {
    constexpr size_t baud_table_size = sizeof(baud_table) / sizeof(baud_entry_t);

    for (size_t i = 0; i < baud_table_size; i++) {
        if (baud_table[i].val == b) {
            return baud_table[i].spd;
        }
    }

    fprintf(stderr, "[!] Warning: unsupported baud rate %d, defaulting to 9600\n", b);
    return B9600;
}

char *parse_terminator(const char *input) {
    if (!input || strcasecmp(input, "none") == 0) {
        return nullptr;
    }
    if (strcasecmp(input, "CRLF") == 0) {
        return strdup("\r\n");
    }
    if (strcasecmp(input, "LF") == 0) {
        return strdup("\n");
    }
    if (strcasecmp(input, "CR") == 0) {
        return strdup("\r");
    }

    /* custom 1-2 char terminator */
    size_t len = strlen(input);

    if (len > 2) {
        fprintf(stderr, "[!] Warning: terminator truncated to 2 chars\n");
        len = 2;
    }

    char *res = malloc(len + 1);
    memcpy(res, input, len);
    res[len] = '\0';

    return res;
}

void setup_port(const int fd, const struct serial_conf *conf) {

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return;
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, conf->baud);
    cfsetospeed(&tty, conf->baud);

    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= (conf->data_bits == 7) ? CS7 : CS8;

    if (conf->parity == 'E') {
        tty.c_cflag |= PARENB;
        tty.c_cflag &= ~PARODD;
        tty.c_iflag |= INPCK;
    }
    else if (conf->parity == 'O') {
        tty.c_cflag |= PARENB;
        tty.c_cflag |= PARODD;
        tty.c_iflag |= INPCK;
    }
    else {
        tty.c_cflag &= ~PARENB;
        tty.c_iflag &= ~INPCK;
    }

    if (conf->stop_bits == 2) {
        tty.c_cflag |= CSTOPB;
    }
    else {
        tty.c_cflag &= ~CSTOPB;
    }

    tty.c_cflag &= ~CRTSCTS;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    switch (conf->flow_ctrl) {
        case 1: /* RTS/CTS hardware */
            tty.c_cflag |= CRTSCTS;
            break;
        case 2: /* XON/XOFF software */
            tty.c_iflag |= (IXON | IXOFF);
            break;
        case 3: /* DTR/DSR – controlled via TIOCM_* ioctls after open */
            /* DTR is raised by clearing HUPCL and setting TIOCM_DTR below */
            tty.c_cflag &= ~HUPCL;
            break;
        default:
            break; /* none */
    }

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10; /* 1 s read timeout */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
    }

    /* Apply DTR/DSR after termios (flow_ctrl == 3) */
    if (conf->flow_ctrl == 3) {
        int mctrl;

        ioctl(fd, TIOCMGET, &mctrl);
        mctrl |= TIOCM_DTR; /* raise DTR to signal "ready" */
        ioctl(fd, TIOCMSET, &mctrl);
    }
}

void manual_control(const int fd, const struct serial_conf *conf) {
    int mctrl;

    if (ioctl(fd, TIOCMGET, &mctrl) < 0) {
        perror("TIOCMGET");
        return;
    }

    if (conf->set_dtr == 1) {
        mctrl |= TIOCM_DTR;
    }
    else if (conf->set_dtr == 0) {
        mctrl &= ~TIOCM_DTR;
    }

    if (conf->set_rts == 1) {
        mctrl |= TIOCM_RTS;
    }
    else if (conf->set_rts == 0) {
        mctrl &= ~TIOCM_RTS;
    }

    if (conf->set_dtr != -1 || conf->set_rts != -1) {
        ioctl(fd, TIOCMSET, &mctrl);
    }

    /* Re-read after possible changes */
    ioctl(fd, TIOCMGET, &mctrl);
    printf("\033[1;33m[MODEM LINES]\033[0m\n");
    printf("  DTR: %s   RTS: %s\n", (mctrl & TIOCM_DTR) ? "\033[32mSET\033[0m" : "\033[31mCLR\033[0m",
           (mctrl & TIOCM_RTS) ? "\033[32mSET\033[0m" : "\033[31mCLR\033[0m");
    printf("  DSR: %s   CTS: %s\n", (mctrl & TIOCM_DSR) ? "\033[32mSET\033[0m" : "\033[31mCLR\033[0m",
           (mctrl & TIOCM_CTS) ? "\033[32mSET\033[0m" : "\033[31mCLR\033[0m");
}

int send_raw(const int fd, const char *data, const char *term) {
    if (!data) {
        return -1;
    }

    if (write(fd, data, strlen(data)) < 0) {
        perror("write");
        return -1;
    }

    if (term && write(fd, term, strlen(term)) < 0) {

        perror("write term");
        return -1;
    }

    return 0;
}

int hex_to_bytes(const char *hex, unsigned char *out, const size_t max) {
    const size_t len = strlen(hex);

    if (len % 2 != 0) {
        fprintf(stderr, "[!] Hex string must have even length\n");
        return -1;
    }

    const size_t n = len / 2;
    if (n > max) {

        fprintf(stderr, "[!] Hex payload too large\n");
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%02x", &byte) != 1) {
            fprintf(stderr, "[!] Invalid hex byte at position %zu\n", 2 * i);
            return -1;
        }

        out[i] = (unsigned char) byte;
    }

    return (int) n;
}

void run_ping(const int fd, const struct serial_conf *conf) {
    char rx_buf[256];
    struct timespec t1, t2;

    printf("\033[1;36m[PING MODE]\033[0m Sending probe to %s...\n", conf->port);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    send_raw(fd, "PING", conf->term);

    fd_set set;
    struct timeval tv = {2, 0};
    FD_ZERO(&set);
    FD_SET(fd, &set);

    if (select(fd + 1, &set, nullptr, nullptr, &tv) > 0) {

        ssize_t n = read(fd, rx_buf, sizeof(rx_buf) - 1);

        if (n > 0) {

            rx_buf[n] = '\0';
            clock_gettime(CLOCK_MONOTONIC, &t2);

            const double rtt = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_nsec - t1.tv_nsec) / 1e6;

            printf("\033[32mReceived:\033[0m \"%s\" | \033[1mRTT: %.3f ms\033[0m\n", rx_buf, rtt);
        }
    }
    else {
        printf("\033[31mTimeout!\033[0m No response within 2 s.\n");
    }
}

void run_transaction(const int fd, const struct serial_conf *conf) {
    if (!conf->msg) {

        fprintf(stderr, "Transaction requires -m <msg>\n");
        return;
    }

    printf("\033[1;35m[TRANSACTION]\033[0m Sending \"%s\" (timeout %d ms)...\n", conf->msg,
           conf->transaction_timeout_ms);

    send_raw(fd, conf->msg, conf->term);

    char rx_buf[1024];
    ssize_t total = 0;
    struct timespec deadline, now;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long) conf->transaction_timeout_ms * 1000000L;

    if (deadline.tv_nsec >= 1000000000L) {

        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    while (total < (ssize_t) (sizeof(rx_buf) - 1)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long ms_left = (deadline.tv_sec - now.tv_sec) * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000;

        if (ms_left <= 0) {
            break;
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv = {ms_left / 1000, (ms_left % 1000) * 1000};

        if (select(fd + 1, &set, nullptr, nullptr, &tv) <= 0) {
            break;
        }

        ssize_t n = read(fd, rx_buf + total, sizeof(rx_buf) - 1 - total);

        if (n <= 0) {
            break;
        }

        total += n;
    }

    if (total > 0) {

        rx_buf[total] = '\0';
        printf("\033[32mResponse:\033[0m \"%s\"\n", rx_buf);
    }
    else {
        printf("\033[31mNo response within %d ms\033[0m\n", conf->transaction_timeout_ms);
    }
}

void run_receiver(const int fd) {
    char buf[256];
    printf("\033[34m[LISTENING]\033[0m Ctrl+C to stop...\n");

    while (keep_running) {
        const ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n > 0) {

            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);

            if (strstr(buf, "PING")) {
                send_raw(fd, "PONG", "\n");
            }
        }

        usleep(10000);
    }

    printf("\n\033[33mStopped.\033[0m\n");
}

typedef struct {
    int fd;
    const char *term;
} rx_thread_arg_t;

void *rx_thread_fn(void *arg) {
    const rx_thread_arg_t *a = (rx_thread_arg_t *) arg;
    unsigned char buffer[8192];
    int bytes_in_buffer = 0;

    while (keep_running) {
        fd_set set;
        struct timeval timeout;

        FD_ZERO(&set);
        FD_SET(a->fd, &set);

        // timeout 20ms
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;

        const int rv = select(a->fd + 1, &set, nullptr, nullptr, &timeout);

        if (rv == -1) {
            if (errno != EINTR) {
                break;
            }
        }
        else if (rv == 0) {
            if (bytes_in_buffer > 0) {
                buffer[bytes_in_buffer] = '\0';

                printf("\r\033[2K\033[32mRX>\033[0m %s\n\033[33mTX>\033[0m ", buffer);
                fflush(stdout);

                if (strstr((char *) buffer, "PING")) {
                    send_raw(a->fd, "PONG", a->term ? a->term : "\n");
                }

                bytes_in_buffer = 0;
            }
        }
        else {

            const int n = read(a->fd, buffer + bytes_in_buffer, sizeof(buffer) - bytes_in_buffer - 1);

            if (n > 0) {

                bytes_in_buffer += n;

                if (bytes_in_buffer >= (int) sizeof(buffer) - 1) {
                    buffer[bytes_in_buffer] = '\0';
                    printf("\r\033[32mRX (full)>\033[0m %s\n", buffer);
                    bytes_in_buffer = 0;
                }
            }
        }
    }

    return nullptr;
}

void run_interactive_text(const int fd, const struct serial_conf *conf) {
    char tx_buf[1024];
    printf("\033[1;33m[TX/RX MODE]\033[0m Type message + Enter to send. Empty line or Ctrl+C to quit.\n");

    rx_thread_arg_t arg = {fd, conf->term};
    pthread_t rx_tid;
    pthread_create(&rx_tid, nullptr, rx_thread_fn, &arg);

    while (keep_running) {

        printf("\033[33mTX>\033[0m ");
        fflush(stdout);

        if (!fgets(tx_buf, sizeof(tx_buf), stdin)) {
            break;
        }

        size_t len = strlen(tx_buf);

        if (len > 0 && tx_buf[len - 1] == '\n') {
            tx_buf[--len] = '\0';
        }
        if (len == 0) {

            keep_running = 0;
            break;
        }

        send_raw(fd, tx_buf, conf->term);
    }

    keep_running = 0;
    pthread_join(rx_tid, nullptr);
    printf("\n\033[33mStopped.\033[0m\n");
}

void run_binary_mode(const int fd, const struct serial_conf *conf) {
    unsigned char tx_bytes[512];
    char hex_input[1025];

    if (conf->msg) {
        /* Send hex string supplied via -m */
        const int n = hex_to_bytes(conf->msg, tx_bytes, sizeof(tx_bytes));
        if (n < 0) {
            return;
        }

        printf("\033[1;35m[BINARY]\033[0m Sending %d bytes: ", n);

        for (int i = 0; i < n; i++) {
            printf("%02X ", tx_bytes[i]);
        }

        printf("\n");
        write(fd, tx_bytes, n);

        if (conf->term) {
            write(fd, conf->term, strlen(conf->term));
        }

        return;
    }

    /* Interactive hex editor */
    printf("\033[1;35m[BINARY MODE]\033[0m Enter bytes as hex pairs (e.g. 41 42 43), empty to quit:\n");
    while (keep_running) {
        printf("\033[35mHEX>\033[0m ");
        fflush(stdout);

        if (!fgets(hex_input, sizeof(hex_input), stdin)) {
            break;
        }
        size_t len = strlen(hex_input);
        if (len > 0 && hex_input[len - 1] == '\n') {
            hex_input[--len] = '\0';
        }
        if (len == 0) {
            break;
        }

        /* Remove spaces to get continuous hex string */
        char compact[1025];
        int ci = 0;
        for (size_t i = 0; i < len; i++) {
            if (hex_input[i] != ' ' && hex_input[i] != '\t') {
                compact[ci++] = hex_input[i];
            }
        }
        compact[ci] = '\0';

        int n = hex_to_bytes(compact, tx_bytes, sizeof(tx_bytes));
        if (n < 0) {
            continue;
        }

        printf("  \033[90m[Sending %d bytes:", n);
        for (int i = 0; i < n; i++) {
            printf(" %02X", tx_bytes[i]);
        }
        printf("]\033[0m\n");

        write(fd, tx_bytes, n);
        if (conf->term) {
            write(fd, conf->term, strlen(conf->term));
        }

        /* Show received bytes in hex */
        fd_set set;
        struct timeval tv = {1, 0};
        FD_ZERO(&set);
        FD_SET(fd, &set);
        if (select(fd + 1, &set, nullptr, nullptr, &tv) > 0) {
            unsigned char rx[256];
            ssize_t rn = read(fd, rx, sizeof(rx));
            if (rn > 0) {
                printf("  \033[32m[RX %zd bytes:", rn);
                for (ssize_t i = 0; i < rn; i++) {
                    printf(" %02X", rx[i]);
                }
                printf("]\033[0m\n");
            }
        }
    }
}

void list_ports() {
    DIR *d = opendir("/dev");
    if (!d) {
        perror("opendir /dev");
        return;
    }

    struct dirent *e;
    int found = 0;
    printf("Available serial ports:\n");

    while ((e = readdir(d))) {

        if (strncmp(e->d_name, "ttyUSB", 6) == 0 || strncmp(e->d_name, "ttyACM", 6) == 0 ||
            strncmp(e->d_name, "ttyS", 4) == 0 || strncmp(e->d_name, "cu.usbmodem", 11) == 0) {
            /* Quick existence check */
            char path[64];
            snprintf(path, sizeof(path), "/dev/%s", e->d_name);
            const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd >= 0) {

                close(fd);
                printf("  %s\n", path);
                found++;
            }
        }
    }

    closedir(d);
    if (!found) {
        printf("  (none found)\n");
    }
}

void print_help(const char *prog) {
    printf("Usage: %s -d <port> [options]\n\n", prog);
    printf("OB (mandatory) parameters:\n");
    printf("  -d <port>     Serial device (e.g. /dev/ttyUSB0)\n");
    printf("  -b <baud>     Baud rate: 150,300,600,1200,2400,4800,9600,19200,38400,57600,115200\n");
    printf("  -s <bits>     Data bits: 7 or 8  (default 8)\n");
    printf("  -p <par>      Parity: N/E/O       (default N)\n");
    printf("  -S <stop>     Stop bits: 1 or 2   (default 1)\n");
    printf("  -f <flow>     Flow control: 0=none 1=RTS/CTS 2=XON/XOFF 3=DTR/DSR (default 0)\n");
    printf("  -t <term>     Terminator: CR LF CRLF or custom 1-2 char (default none)\n");
    printf("  -m <msg>      Message to send (text or hex in binary mode)\n");
    printf("  -l            List available serial ports\n");
    printf("  -h            This help\n");
    printf("\nModes:\n");
    printf("  (default)     Interactive text TX/RX mode     [OB 6.1]\n");
    printf("  --listen      Receive only, no TX             [OB 3]\n");
    printf("  --ping        PING round-trip test            [OB 5]\n");
    printf("  --binary      Binary (hex) TX/RX mode         [OP 6.2]\n");
    printf("  --transaction Transaction with timeout        [OP 4]\n");
    printf("  --timeout <ms>  Timeout for transaction in ms (default 2000)\n");
    printf("  --set-dtr <0|1> Set/clear DTR line            [OP 1.4]\n");
    printf("  --set-rts <0|1> Set/clear RTS line            [OP 1.4]\n");
    printf("  --monitor     Show modem line status          [OP 1.4]\n");
}

/* ----------------------------------------------------------------------- */
/* main                                                                     */
/* ----------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);

    struct serial_conf conf = {
        .port = nullptr,
        .msg = nullptr,
        .term = nullptr,
        .do_ping = 0,
        .do_transaction = 0,
        .transaction_timeout_ms = 2000,
        .do_manual_ctrl = 0,
        .set_dtr = -1,
        .set_rts = -1,
        .binary_mode = 0,
        .do_listen = 0,
        .baud = B9600,
        .data_bits = 8,
        .parity = 'N',
        .stop_bits = 1,
        .flow_ctrl = 0,
    };

    enum { OPT_PING = 256, OPT_BIN, OPT_TRANS, OPT_TIMEOUT, OPT_DTR, OPT_RTS, OPT_MONITOR, OPT_LISTEN };

    static struct option long_opts[] = {{"device", 1, nullptr, 'd'},
                                        {"baud", 1, nullptr, 'b'},
                                        {"msg", 1, nullptr, 'm'},
                                        {"term", 1, nullptr, 't'},
                                        {"bits", 1, nullptr, 's'},
                                        {"parity", 1, nullptr, 'p'},
                                        {"stop", 1, nullptr, 'S'},
                                        {"flow", 1, nullptr, 'f'},
                                        {"list", 0, nullptr, 'l'},
                                        {"help", 0, nullptr, 'h'},
                                        {"ping", 0, nullptr, OPT_PING},
                                        {"binary", 0, nullptr, OPT_BIN},
                                        {"transaction", 0, nullptr, OPT_TRANS},
                                        {"timeout", 1, nullptr, OPT_TIMEOUT},
                                        {"set-dtr", 1, nullptr, OPT_DTR},
                                        {"set-rts", 1, nullptr, OPT_RTS},
                                        {"monitor", 0, nullptr, OPT_MONITOR},
                                        {"listen", 0, nullptr, OPT_LISTEN},
                                        {nullptr, 0, nullptr, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "d:b:m:t:s:p:S:f:lh", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'd':
                conf.port = strdup(optarg);
                break;
            case 'b':
                conf.baud = baud_from_int(atoi(optarg));
                break;
            case 'm':
                conf.msg = strdup(optarg);
                break;
            case 't':
                conf.term = parse_terminator(optarg);
                break;
            case 's':
                conf.data_bits = atoi(optarg);
                break;
            case 'p':
                conf.parity = (optarg[0] >= 'a') ? optarg[0] - 32 : optarg[0];
                break;
            case 'S':
                conf.stop_bits = atoi(optarg);
                break;
            case 'f':
                conf.flow_ctrl = atoi(optarg);
                break;
            case 'l':
                list_ports();
                return 0;
            case 'h':
                print_help(argv[0]);
                return 0;
            case OPT_PING:
                conf.do_ping = 1;
                break;
            case OPT_BIN:
                conf.binary_mode = 1;
                break;
            case OPT_TRANS:
                conf.do_transaction = 1;
                break;
            case OPT_TIMEOUT:
                conf.transaction_timeout_ms = atoi(optarg);
                break;
            case OPT_DTR:
                conf.set_dtr = atoi(optarg);
                conf.do_manual_ctrl = 1;
                break;
            case OPT_RTS:
                conf.set_rts = atoi(optarg);
                conf.do_manual_ctrl = 1;
                break;
            case OPT_MONITOR:
                conf.do_manual_ctrl = 1;
                break;
            case OPT_LISTEN:
                conf.do_listen = 1;
                break;
            default:
                fprintf(stderr, "Unknown option. Use -h for help.\n");
                return 1;
        }
    }

    if (!conf.port) {

        fprintf(stderr, "Error: device port is required (-d <port>). Use -h for help.\n");
        return 1;
    }

    int fd = open(conf.port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {

        fprintf(stderr, "Error opening %s: %s\n", conf.port, strerror(errno));
        return 1;
    }

    /* Restore blocking mode for reads */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    setup_port(fd, &conf);

    /* Dispatch */
    if (conf.do_manual_ctrl) {
        manual_control(fd, &conf);
    }
    else if (conf.do_ping) {
        run_ping(fd, &conf);
    }
    else if (conf.do_transaction) {
        run_transaction(fd, &conf);
    }
    else if (conf.binary_mode) {
        run_binary_mode(fd, &conf);
    }
    else if (conf.msg) {
        /* Quick one-shot text send */
        send_raw(fd, conf.msg, conf.term);
        printf("Message sent to %s\n", conf.port);
    }
    else if (conf.do_listen) {
        run_receiver(fd);
    }
    else {
        run_interactive_text(fd, &conf);
    }

    close(fd);
    free(conf.port);

    if (conf.msg) {
        free(conf.msg);
    }
    if (conf.term) {
        free(conf.term);
    }

    return 0;
}
