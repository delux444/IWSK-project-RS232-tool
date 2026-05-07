#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>



enum mode{
    RECEIVE, SEND, NOACTION
};
enum parity{
    N, E, O, UNKNOWN //dodatkowy bit - ustawiamy wartosc, aby laczna ilosc jedynek byla parzysta lub nieparzysta
};
enum control{ //zeby nadawca nie wysylal danych za szybko wzgledem odbierajacego
    NOCONTROL, SOFTCONTROL, HARDCONTROL

    //soft - IXOn i IXOFF
    //OFF - gdy odbiorca jest prawie pelny
    //potem odbiorca wysle on, gdy bedzie miec miejsce


    //hard - RTS-CTS
    //dedykowane fizyczne piny
    //RTS (Request to Send) lub CTS (Clear to Send)
    //rts - nadawca chce wyslac
   //cts - odbiorca jest gotowy, ma wolne miejsce w buforze

   //laczone sa na krzyz

};
struct device{
    enum mode md;
    char *terminator;
    char *name;
    char *data;
};
struct userinput{
    enum parity par;
    enum control con;
    speed_t baudrate; //zamiast liczb wpisuje sie stale np. B9600
    tcflag_t databits; //przechowuje flagi konfiguracyjne terminala - okreslaja liczbe bitow w ramce danych (CS5, CS6, CS7, CS8) - 5 - 8 bitow
    int stopbits;
};

/* INPUT VALIDATION */
int handle_input(int, char *[], struct device *, struct userinput *);
void free_device(struct device *);
speed_t parse_baudrate(const char *);
tcflag_t parse_databits(const char *);
enum parity parse_parity(const char *);
char* parse_terminator(const char *);
enum mode parse_mode(const char *);
void list_serial_devices();

/* TERMIOS CONFIG */
void setup_termios(struct termios *, struct userinput *);
int write_all(int, const char *, size_t);
int write_to_device(int, char *, char *);
void listen_device(int);

/* DEBUG FUNCTIONS */
void print_device(struct device *);
void print_userinput(struct userinput *);
const char* parity_to_string(enum parity);
const char* control_to_string(enum control);

void print_help(const char *);

volatile int stop = 0;
void handle_sigint(int sig) {
    stop = 1;
}

int main(int argc, char *argv[]){
    signal(SIGINT, handle_sigint);

    struct termios term;
    struct device dev = {NOACTION, NULL, NULL, NULL};
    struct userinput usr = {N, NOCONTROL, B9600, CS7, 1};

    if(handle_input(argc, argv, &dev, &usr) == -1) { return -1; }

    //verifying if interface exists

    if (access(dev.name, F_OK) == -1) {
        fprintf(stderr, "Error: Device '%s' does not exist.\n", dev.name);
        return -1;
    }

    if (access(dev.name, R_OK | W_OK) == -1) {
        fprintf(stderr, "Error: You don't have read/write permissions for '%s'.\n", dev.name);
        fprintf(stderr, "Hint: Try running with 'sudo' or check your user groups.\n");
        return -1;
    }

    if(!dev.name){
        fprintf(stderr, "Device not specified\n");
        return -1;
    }

    //otwieramy plik, read and write, NOCTTY - niezaleznosc od urzadzenia, do ktorego sie podlaczymy
    int fd = open(dev.name, O_RDWR | O_NOCTTY);
    if(fd < 0){
        perror("open failed");
        return -1;
    }

    if(tcgetattr(fd, &term) < 0){
        perror("tcgetattr failed");
        close(fd);
        return -1;
    }

    setup_termios(&term, &usr);

    if(tcsetattr(fd, TCSANOW, &term) < 0){
        perror("tcsetattr failed");
        close(fd);
        return -1;
    }

    /* DEBUG FUNCTIONS */
    print_device(&dev);
    print_userinput(&usr);

    if(dev.md == SEND && dev.data){
        if(write_to_device(fd, dev.data, dev.terminator) < 0){
            close(fd);
            free_device(&dev);
            return -1;
        }
    }
    else if(dev.md == RECEIVE){
        listen_device(fd);
    }

    close(fd);
    free_device(&dev);
    return 0;
}

/* INPUT VALIDATION */
int handle_input(int argc, char *argv[], struct device *dev, struct userinput *usr){

    //slownik wszystkich opcji, ktore uzytkownik moze podac: dlugie argumenty od 2 myslnikow
    //2 arg - czy po nazwie wymagana jest wartosc
    //3 arg - flaga - gdy = 0 to getopt_long zwroci wartosc z czwartego pola
    //gdy zamiast zera wskaznik na int, to getopt_long ustawi ten int na wartosc z czwartego pola i zwroci 0
    struct option longopts[] = {
        {"device", required_argument, 0, 'd'},
        {"baudrate", required_argument, 0, 'b'},
        {"databits", required_argument, 0, 'D'},
        {"parity", required_argument, 0, 'p'},
        {"stopbits", required_argument, 0, 's'},
        {"softcontrol", no_argument, 0, 'S'},
        {"hardcontrol", no_argument, 0, 'H'},
        {"terminator", required_argument, 0, 't'},
        {"transfer", required_argument, 0, 'T'},
        {"mode", required_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {"list", no_argument, 0, 'l'},
        {0, 0, 0, 0} //straznik, ze jest koniec listy
    };

    int opt;
    int optidx;

    //wywolemy funkcje, a wynik do opt
    //gdy wszystkie opcje oblsuzymy opt = -1, koniec petli

    //flagi z : za litera - dodatkowa wartosc do podania
    //h - bez : - flaga, nie wymaga dodatkowej wartosci

    //longopts - tablica z definicjami dlugich opcji
    //&optidx - funkcje zapisuje numer indeksu opcji znalezionej w tablicylongopts
    while( (opt = getopt_long(argc, argv, "d:b:D:p:s:S:H:t:T:m:hl", longopts, &optidx)) != -1 ){

        switch(opt){

            //opttarg - wartosc nadana po opcji/napisie
            case 'd':
                dev->name = malloc(strlen(optarg) + 1);
                if(!dev->name) { return -1; } //nie zaalokowal pamieci
                strcpy(dev->name, optarg);
                break;

            case 'b':
                usr->baudrate = parse_baudrate(optarg);
                if(usr->baudrate == B0){ fprintf(stderr, "Invalid baudrate: %s\n", optarg); return -1;}
                break;
            case 'D':
                usr->databits = parse_databits(optarg);
                if(usr->databits == (tcflag_t)-1) { fprintf(stderr, "Invalid databits: %s\n", optarg); return -1;}
                break;

            case 'p':
                usr->par = parse_parity(optarg);
                if(usr->par == UNKNOWN) { fprintf(stderr, "Unknown parity: %s\n", optarg); return -1; }
                break;

            case 's':
                if(strcmp(optarg, "1") == 0) { usr->stopbits = 1; }
                else if(strcmp(optarg, "2") == 0) { usr->stopbits = 2; }
                else { fprintf(stderr, "Invalid stopbits: %s\n", optarg); return -1; }
                break;

            case 'S':
                usr->con = SOFTCONTROL;
                break;

            case 'H':
                usr->con = HARDCONTROL;
                break;

            case 't':
            {
                char *tmp = parse_terminator(optarg);
                if(!tmp) { fprintf(stderr, "Invalid terminator\n"); return -1; }
                free(dev->terminator);
                dev->terminator = tmp;
                break;
            }
            case 'T':
            {
                //tresc wiadomosco, ktora chces wypisac
                char *tmp = malloc(strlen(optarg) + 1); //plus jeden bo strlen nie zalicza zera na koncu stringa
                if(!tmp) return -1;
                strcpy(tmp, optarg);

                free(dev->data);
                dev->data = tmp;
                break;
            }

            case 'm':
            {
                enum mode tmp = parse_mode(optarg);
                if(tmp == NOACTION && strcmp(optarg, "none") != 0){ fprintf(stderr, "Invalid mode: %s\n", optarg); return -1; }
                dev->md = tmp;
                break;
            }
            case 'h':
                print_help(argv[0]);
                return -1;

            case 'l':
                list_serial_devices();
                return -1;  
            default:
                fprintf(stderr, "Unknown option: %c\n", opt);
                return -1;
                break;
        }
    }

    if (!dev->name) {
        fprintf(stderr, "Missing required option: --device\n");
        return -1;
    }

    return 0;
}
void free_device(struct device *dev){

    if(dev->terminator != NULL) { free(dev->terminator); }
    if(dev->name != NULL) { free(dev->name); }
    if(dev->data != NULL) { free(dev->data); }

}
speed_t parse_baudrate(const char *str){
    if(strcmp(str, "150") == 0) return B150;
    else if(strcmp(str, "200") == 0) return B200;
    else if(strcmp(str, "300") == 0) return B300;
    else if(strcmp(str, "600") == 0) return B600;
    else if(strcmp(str, "1200") == 0) return B1200;
    else if(strcmp(str, "1800") == 0) return B1800;
    else if(strcmp(str, "2400") == 0) return B2400;
    else if(strcmp(str, "4800") == 0) return B4800;
    else if(strcmp(str, "9600") == 0) return B9600;
    else if(strcmp(str, "19200") == 0) return B19200;
    else if(strcmp(str, "38400") == 0) return B38400;
    else if(strcmp(str, "57600") == 0) return B57600;
    else if(strcmp(str, "76800") == 0) return B76800;
    else if(strcmp(str, "115200") == 0) return B115200;
    return B0; // invalid
}
tcflag_t parse_databits(const char *baud){
    if(strcmp(baud, "5") == 0) { return CS5; }
    else if(strcmp(baud, "6") == 0) {return CS6; }
    else if(strcmp(baud, "7") == 0) {return CS7; }
    else if(strcmp(baud, "8") == 0) {return CS8; }

    return (tcflag_t)-1; // invalid
}
enum parity parse_parity(const char *str){
    if(strcmp(str, "N") == 0 || strcmp(str, "none") == 0) { return N; }
    else if(strcmp(str, "E") == 0 || strcmp(str, "even") == 0) { return E; }
    else if(strcmp(str, "O") == 0 || strcmp(str, "odd") == 0) { return O; }
    return UNKNOWN;
}
char* parse_terminator(const char *str){
    if(strcmp(str, "LF") == 0) { return strdup("\n"); } //strdup -string duplicate
    else if(strcmp(str, "CR") == 0) { return strdup("\r"); }
    else if(strcmp(str, "CRLF") == 0) { return strdup("\r\n"); }

    return NULL;
}
enum mode parse_mode(const char *str){
    if(strcmp(str, "receive") == 0 || strcmp(str, "recv") == 0) { return RECEIVE; }
    else if(strcmp(str, "send") == 0) { return SEND; }
    else if(strcmp(str, "none") == 0) { return NOACTION; }

    return NOACTION;
}

/* TERMIOS CONFIG */
void setup_termios(struct termios *term, struct userinput *usr){

    cfmakeraw(term);  // ❗ ważne

    //predkosc wejsciowa i wyjsciowa - takie same (jedno urzadzenie, dwa kierunki)
    cfsetispeed(term, usr->baudrate); //listen
    cfsetospeed(term, usr->baudrate);//speak

    // liczba bitow danych
    term->c_cflag &= ~CSIZE;
    term->c_cflag |= usr->databits;
    term->c_cc[VMIN] = 0;

    // parity
    if(usr->par == N) {
        term->c_cflag &= ~PARENB;
    } else {
        term->c_cflag |= PARENB;
        if (usr->par == O)
            term->c_cflag |= PARODD;
        else
            term->c_cflag &= ~PARODD;
    }

    // stop bits
    if(usr->stopbits == 2)
        term->c_cflag |= CSTOPB;
    else
        term->c_cflag &= ~CSTOPB;

    // flow control
    if(usr->con == SOFTCONTROL)
        term->c_iflag |= (IXON | IXOFF);
    else
        term->c_iflag &= ~(IXON | IXOFF);

    #ifdef CRTSCTS //this is not standar
    if(usr->con == HARDCONTROL)
        term->c_cflag |= CRTSCTS;
    else
        term->c_cflag &= ~CRTSCTS;
    #endif

    term->c_cflag |= (CLOCAL | CREAD);
    //CREAD - zasilanie dla odbiornika, zebysmy my mogli odbierac dane
    //CLOCAL - ignorowanie linii DCD - w starych modemach

}
int write_to_device(int fd, char *data, char *terminator){

    //najpierw wysylamy dane, potem terminator
    if(data){
        if(write_all(fd, data, strlen(data)) < 0) { return -1; }
    }

    if(terminator){
        if (write_all(fd, terminator, strlen(terminator)) < 0) { return -1; }
    }

    return 0;
}
int write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;

    while(total < len){
        ssize_t n = write(fd, buf + total, len - total);
        if(n <= 0) { perror("write failed"); return -1; }
        total += n;
    }
    return 0;
}
void listen_device(int fd) {
    char buf[256];

    //fd - deskryptor portu szeregowego

    while(stop == 0){

        ssize_t n = read(fd, buf, sizeof(buf));

        if(n > 0){
            ssize_t written = 0;
            while(written < n){

                //poczatek i liczba znakow jakie zostaly do wyswietlenia
                ssize_t w = write(STDOUT_FILENO, buf + written, n - written);
                if(w <= 0){ perror("stdout write failed"); return; }
                written += w;
            }
        }
        else if (n < 0) { perror("read failed"); return; }
    }
}

void print_help(const char *prog){


    //ANSI escape codes for colors and formatting
    #define C_RESET   "\033[0m"
    #define C_BOLD    "\033[1m"
    #define C_GREEN   "\033[32m"
    #define C_YELLOW  "\033[33m"
    #define C_BLUE    "\033[34m"
    #define C_CYAN    "\033[36m"
    #define C_RED     "\033[31m"

    printf(C_BOLD C_BLUE "Serial Tool (RS232/UART)\n" C_RESET);
    printf("Usage: %s [OPTIONS]\n\n", prog);

    printf(C_BOLD C_CYAN "Required:\n" C_RESET);
    printf("  -d, --device <path>        Serial device (e.g. /dev/ttyUSB0)\n");

    printf(C_BOLD C_CYAN "\nConfiguration:\n" C_RESET);
    printf("  -b, --baudrate <rate>      Baudrate (e.g. 9600, 115200)\n");
    printf("  -D, --databits <5|6|7|8>   Number of data bits\n");
    printf("  -p, --parity <N|E|O>       Parity: None, Even, Odd\n");
    printf("  -s, --stopbits <1|2>       Stop bits\n");

    printf(C_BOLD C_CYAN "\nFlow control:\n" C_RESET);
    printf("  -S, --softcontrol          Software flow control (XON/XOFF)\n");
    printf("  -H, --hardcontrol          Hardware flow control (RTS/CTS)\n");

    printf(C_BOLD C_CYAN "\nTransfer:\n" C_RESET);
    printf("  -T, --transfer <data>      Data to send\n");
    printf("  -t, --terminator <CR|LF|CRLF>\n");

    printf(C_BOLD C_CYAN "\nMode:\n" C_RESET);
    printf("  -m, --mode <send|recv|listen|none>\n");

    printf(C_BOLD C_CYAN "\nListing:\n" C_RESET);
    printf("  -l, --list                 List available serial devices in /dev\n");

    printf(C_BOLD C_CYAN "\nExamples:\n" C_RESET);
    printf("  %s -d /dev/ttyUSB0 -b 9600 -m listen\n", prog);
    printf("  %s -d /dev/ttyUSB0 -m send -T \"hello\" -t CRLF\n", prog);

    printf(C_BOLD C_YELLOW "\nAbout RS232 / UART:\n" C_RESET);
    printf("  RS232 is a serial communication standard used to transmit data\n");
    printf("  between devices (e.g. PC <-> microcontroller).\n\n");

    printf("  Data is sent bit by bit:\n");
    printf("    [START] [DATA BITS] [PARITY] [STOP]\n\n");

    printf("  Example (8N1):\n");
    printf("    1 start bit, 8 data bits, no parity, 1 stop bit\n\n");

    printf("  Common parameters:\n");
    printf("    Baudrate  - speed (e.g. 9600 bps)\n");
    printf("    Databits  - size of character (5-8 bits)\n");
    printf("    Parity    - error checking (optional)\n");
    printf("    Stopbits  - end of frame\n\n");

    printf(C_BOLD C_GREEN "Tips:\n" C_RESET);
    printf("  - Use CRLF to avoid overwriting lines\n");
    printf("  - Use socat to create virtual serial ports\n");
    printf("  - Use 'receive' mode to monitor incoming data\n");

    printf("\n");

   

}

/* DEBUG FUNCTIONS */
void print_device(struct device *dev){
    printf("[ DEVICE ]\n");
    switch(dev->md){
        case RECEIVE:
            printf("Mode: RECEIVE\n");
            break;

        case SEND:
            printf("Mode: SEND\n");
            break;

        case NOACTION:
            printf("Mode: NOACTION\n");
            break;

        default:
            printf("Mode: UNKNOWN\n");
            break;
    }

    printf("Device name: ");
    dev->name != NULL ? printf("%s\n", dev->name) : printf("NULL\n");

    printf("Terminator: ");
    dev->terminator != NULL ? printf("%s\n", dev->terminator) : printf("NULL\n");

    printf("Data: ");
    dev->data != NULL ? printf("%s\n", dev->data) : printf("NULL\n");

}
void print_userinput(struct userinput *usr){

    printf("[ USER INPUT ]\n");

    printf("%lu\n", (unsigned long)usr->baudrate);
    printf("Databits: ");

    switch(usr->databits){ //predkosci
        case CS5: printf("5\n"); break;
        case CS6: printf("6\n"); break;
        case CS7: printf("7\n"); break;
        case CS8: printf("8\n"); break;
        default: printf("Unknown\n"); break;
    }

    printf("Stopbits: %d\n", usr->stopbits);

    printf("Parity: %s\n", parity_to_string(usr->par));
    printf("Control: %s\n", control_to_string(usr->con));
}
const char* parity_to_string(enum parity p){
    switch(p){
        case N: return "None";
        case E: return "Even";
        case O: return "Odd";
        default: return "Unknown";
    }
}
const char* control_to_string(enum control c){
    switch(c){
        case NOCONTROL: return "No control";
        case SOFTCONTROL: return "Software";
        case HARDCONTROL: return "Hardware";
        default: return "Unknown";
    }
}





void list_serial_devices() {
    DIR *dir;
    struct dirent *entry;

    printf("Available serial devices in /dev/:\n");
    dir = opendir("/dev");
    if (dir == NULL) {
        perror("Unable to open /dev");
        return;
    }

    int found = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;

        // Combined Linux and macOS naming conventions
        if (strncmp(name, "ttyUSB", 6) == 0 ||   // Linux: USB-Serial adapters
            strncmp(name, "ttyACM", 6) == 0 ||   // Linux: USB Modems/Arduinos
            strncmp(name, "ttyS", 4) == 0 ||     // Linux: Hardware Serial ports
            strncmp(name, "cu.", 3) == 0 ||      // Mac: Call-out devices (Preferred)
            strncmp(name, "tty.", 4) == 0)       // Mac: Interactive terminal devices
        {
            // Filter out common internal Mac noise (Bluetooth/AirPort/Internal Modems)
            if (strstr(name, "Bluetooth") == NULL && 
                strstr(name, "Wireless") == NULL && 
                strstr(name, "Soc") == NULL) 
            {
                printf("  /dev/%s\n", name);
                found = 1;
            }
        }
    }

    if (!found) {
        printf("  No serial devices found. Check your connection or drivers.\n");
    }

    closedir(dir);
}


