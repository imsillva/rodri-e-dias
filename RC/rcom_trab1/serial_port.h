#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stddef.h>
#include <termios.h>

int serial_open_and_configure(const char *port_name, struct termios *oldtio);
int serial_restore_and_close(int fd, const struct termios *oldtio);
int serial_write_all(int fd, const unsigned char *buf, size_t len);
int serial_read_byte(int fd, unsigned char *byte);

#endif
