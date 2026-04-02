#define _POSIX_C_SOURCE 200809L
#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define BAUDRATE B38400

int serial_open_and_configure(const char *port_name, struct termios *oldtio)
{
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror(port_name);
        return -1;
    }

    struct termios newtio;
    if (tcgetattr(fd, oldtio) == -1) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 1;
    newtio.c_cc[VMIN] = 0;

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        perror("fcntl");
        tcsetattr(fd, TCSANOW, oldtio);
        close(fd);
        return -1;
    }

    return fd;
}

int serial_restore_and_close(int fd, const struct termios *oldtio)
{
    int status = 0;
    if (fd >= 0 && oldtio != NULL) {
        if (tcsetattr(fd, TCSANOW, oldtio) == -1) {
            perror("tcsetattr");
            status = -1;
        }
    }
    if (fd >= 0 && close(fd) == -1) {
        perror("close");
        status = -1;
    }
    return status;
}

int serial_write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t res = write(fd, buf + total, len - total);
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write");
            return -1;
        }
        if (res == 0) {
            fprintf(stderr, "write returned 0\n");
            return -1;
        }
        total += (size_t)res;
    }

    if (tcdrain(fd) == -1) {
        perror("tcdrain");
        return -1;
    }

    return 0;
}

int serial_read_byte(int fd, unsigned char *byte)
{
    int res = (int)read(fd, byte, 1);
    if (res < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return 0;
        }
        perror("read");
        return -1;
    }
    return res;
}
