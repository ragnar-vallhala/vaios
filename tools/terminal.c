#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <ctype.h>
#include <signal.h>

// ------------------------------------
// RAW MODE
// ------------------------------------
struct termios old_stdin;

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &old_stdin);

    struct termios raw = old_stdin;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG); 
    // ICANON = disable line buffering
    // ECHO   = disable local terminal echo
    // ISIG   = disable Ctrl+C, Ctrl+Z signals

    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void restore_stdin() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_stdin);
}

// ------------------------------------
// CLEAR SCREEN
// ------------------------------------
void clear_screen() {
    printf("\033[2J\033[H");
    fflush(stdout);
}

// ------------------------------------
// SERIAL SETUP
// ------------------------------------
int set_interface_attribs(int fd, int speed) {
    struct termios tty;
    if (tcgetattr(fd, &tty) < 0) {
        perror("tcgetattr");
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }
    return 0;
}

int convert_baud(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:
            fprintf(stderr, "Unsupported baud: %d\n", baud);
            exit(1);
    }
}

// ------------------------------------
// MAIN
// ------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <serial_port> <baud>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, SIG_IGN);   // ignore Ctrl+C in terminal

    enable_raw_mode();
    atexit(restore_stdin);

    const char *port = argv[1];
    int baud = atoi(argv[2]);
    int speed = convert_baud(baud);

    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (set_interface_attribs(fd, speed) < 0)
        return 1;

    printf("Connected to %s at %d baud\n", port, baud);
    printf("Press Ctrl+X to quit.\n");
    printf("Type /clear to clear screen.\n\n");
    fflush(stdout);

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
    fds[1].fd = fd;           fds[1].events = POLLIN;

    char buf[256];

    while (1) {
        poll(fds, 2, -1);

        // KEYBOARD INPUT
        if (fds[0].revents & POLLIN) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));

            for (int i = 0; i < n; i++) {

                // Ctrl+X exit (0x18)
                if (buf[i] == 0x18) {
                    printf("\nExiting...\n");
                    close(fd);
                    return 0;
                }

                // Backspace handling
                if (buf[i] == 0x7F) {
                    write(STDOUT_FILENO, "\b \b", 3); // erase locally
                    write(fd, "\b", 1);              // send to device if needed
                    continue;
                }

                // /clear
                if (strncmp(buf, "/clear", 6) == 0) {
                    clear_screen();
                    break;
                }

                // Local echo (optional)
                write(STDOUT_FILENO, &buf[i], 1);

                // Send to serial
                if (buf[i] == '\n')
                    write(fd, "\r\n", 2);
                else
                    write(fd, &buf[i], 1);
            }
        }

        // SERIAL INPUT
        if (fds[1].revents & POLLIN) {
            int n = read(fd, buf, sizeof(buf));
            if (n > 0)
                write(STDOUT_FILENO, buf, n);
        }
    }
}
