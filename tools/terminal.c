#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <ctype.h>

void clear_screen() {
    printf("\033[2J\033[H"); // ANSI clear + cursor home
    fflush(stdout);
}

int set_interface_attribs(int fd, int speed) {
    struct termios tty;
    if (tcgetattr(fd, &tty) < 0) {
        perror("tcgetattr");
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;   // 8-bit chars
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;  // no canonical mode, no echo

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
        default:
            fprintf(stderr, "Unsupported baud: %d\n", baud);
            exit(1);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <serial_port> <baud>\n", argv[0]);
        return 1;
    }

    const char *port = argv[1];
    int baud = atoi(argv[2]);

    int speed = convert_baud(baud);

    // Open serial port
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (set_interface_attribs(fd, speed) < 0) {
        return 1;
    }

    printf("Connected to %s at %d baud\n", port, baud);
    printf("Type clear to clear screen\n\n");
    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
    fds[1].fd = fd;           fds[1].events = POLLIN;

    char buf[256];

    while (1) {
        poll(fds, 2, -1);

        // --- Keyboard input ---
        if (fds[0].revents & POLLIN) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                // Check for command
                if (strncmp(buf, "/clear", 6) == 0) {
                    clear_screen();
                    continue;
                }

                // Convert Enter → CRLF
                for (int i = 0; i < n; i++) {
                    if (buf[i] == '\n') {
                        write(fd, "\r\n", 2);   // send CRLF
                    } else {
                        write(fd, &buf[i], 1);      // send
                    }
                }
            }
        }

        // --- Serial device input ---
        if (fds[1].revents & POLLIN) {
            int n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                write(STDOUT_FILENO, buf, n);  // print device data
            }
          }
    }

    close(fd);
    return 0;
}

