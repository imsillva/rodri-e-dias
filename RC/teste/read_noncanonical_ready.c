#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define BAUDRATE B38400
#define _POSIX_SOURCE 1

#define FALSE 0
#define TRUE 1

#define MAX_DATA_SIZE 2048
#define SU_FRAME_SIZE 5
#define MAX_PATH_LEN 512

#define FLAG    0x7E
#define ESC     0x7D
#define ESC_XOR 0x20

#define A_TX_CMD   0x03
#define A_RX_REPLY 0x03

#define C_SET  0x03
#define C_UA   0x07
#define C_DISC 0x0B
#define C_I0   0x00
#define C_I1   0x40
#define C_RR0  0x05
#define C_RR1  0x85
#define C_REJ0 0x01
#define C_REJ1 0x81

#define APP_DATA  0x01
#define APP_START 0x02
#define APP_END   0x03
#define APP_T_SIZE 0x00
#define APP_T_NAME 0x01

typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    BCC1_OK,
    DATA_RCV,
    ESC_RCV
} state_t;

typedef enum {
    DISCONNECTED,
    CONNECTED,
    DISCONNECTING
} conn_state_t;

typedef struct {
    FILE *fp;
    char output_dir[MAX_PATH_LEN];
    char file_name[256];
    char output_path[MAX_PATH_LEN];
    size_t expected_size;
    size_t bytes_written;
    int started;
    int ended;
} app_state_t;

static int write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t res = write(fd, buf + total, len - total);
        if (res < 0) {
            if (errno == EINTR)
                continue;
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

static int read_byte(int fd, unsigned char *byte)
{
    int res = read(fd, byte, 1);
    if (res < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return 0;
        perror("read");
        return -1;
    }
    return res;
}

static int configure_port(int fd, struct termios *oldtio)
{
    struct termios newtio;
    if (tcgetattr(fd, oldtio) == -1) {
        perror("tcgetattr");
        return -1;
    }
    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag     = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag     = IGNPAR;
    newtio.c_oflag     = 0;
    newtio.c_lflag     = 0;
    newtio.c_cc[VTIME] = 1;
    newtio.c_cc[VMIN]  = 0;
    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        return -1;
    }
    return 0;
}

static int send_supervision(int fd, unsigned char A, unsigned char C)
{
    unsigned char frame[SU_FRAME_SIZE] = { FLAG, A, C, (unsigned char)(A ^ C), FLAG };
    printf("TX S-frame: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
           frame[0], frame[1], frame[2], frame[3], frame[4]);
    fflush(stdout);
    return write_all(fd, frame, SU_FRAME_SIZE);
}

static const char *safe_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;

    if (slash != NULL && slash[1] != '\0')
        base = slash + 1;
    if (backslash != NULL && backslash[1] != '\0' && backslash + 1 > base)
        base = backslash + 1;
    return base;
}

static size_t decode_size_value(const unsigned char *buf, int len)
{
    size_t value = 0;
    for (int i = 0; i < len; i++)
        value = (value << 8) | buf[i];
    return value;
}

static int parse_control_packet(const unsigned char *payload,
                                int payload_len,
                                unsigned char expected_control,
                                char *file_name,
                                size_t file_name_size,
                                size_t *file_size)
{
    if (payload_len < 1 || payload[0] != expected_control)
        return -1;

    int idx = 1;
    int saw_name = FALSE;
    int saw_size = FALSE;

    while (idx + 2 <= payload_len) {
        unsigned char type = payload[idx++];
        unsigned char len  = payload[idx++];
        if (idx + len > payload_len)
            return -1;

        if (type == APP_T_SIZE) {
            *file_size = decode_size_value(&payload[idx], len);
            saw_size = TRUE;
        } else if (type == APP_T_NAME) {
            size_t copy_len = (len < file_name_size - 1) ? len : file_name_size - 1;
            memcpy(file_name, &payload[idx], copy_len);
            file_name[copy_len] = '\0';
            saw_name = TRUE;
        }
        idx += len;
    }

    return (saw_name && saw_size) ? 0 : -1;
}

static int process_app_packet(const unsigned char *payload,
                              int payload_len,
                              app_state_t *app)
{
    if (payload_len < 1)
        return -1;

    unsigned char control = payload[0];

    if (control == APP_START) {
        char received_name[256] = {0};
        size_t received_size = 0;
        if (parse_control_packet(payload, payload_len, APP_START,
                                 received_name, sizeof(received_name),
                                 &received_size) < 0) {
            fprintf(stderr, "Invalid START packet\n");
            return -1;
        }

        const char *base = safe_basename(received_name);
        snprintf(app->file_name, sizeof(app->file_name), "%s", base);
        if (strlen(app->output_dir) + 4 + strlen(app->file_name) + 1 >= sizeof(app->output_path)) {
            fprintf(stderr, "Output path too long\n");
            return -1;
        }
        snprintf(app->output_path, sizeof(app->output_path), "%s/rx_%s",
                 app->output_dir, app->file_name);
        app->expected_size = received_size;
        app->bytes_written = 0;
        app->ended = FALSE;

        if (app->fp != NULL) {
            fclose(app->fp);
            app->fp = NULL;
        }

        app->fp = fopen(app->output_path, "wb");
        if (app->fp == NULL) {
            perror(app->output_path);
            return -1;
        }

        app->started = TRUE;
        printf("START packet: file=%s size=%zu -> writing to %s\n",
               app->file_name, app->expected_size, app->output_path);
        fflush(stdout);
        return 0;
    }

    if (control == APP_DATA) {
        if (!app->started || app->fp == NULL) {
            fprintf(stderr, "DATA packet received before START\n");
            return -1;
        }
        if (payload_len < 3) {
            fprintf(stderr, "Invalid DATA packet\n");
            return -1;
        }

        int k = ((int)payload[1] << 8) | payload[2];
        if (payload_len != 3 + k) {
            fprintf(stderr, "DATA length mismatch: header=%d actual=%d\n",
                    k, payload_len - 3);
            return -1;
        }

        size_t written = fwrite(&payload[3], 1, (size_t)k, app->fp);
        if (written != (size_t)k) {
            perror("fwrite");
            return -1;
        }
        app->bytes_written += written;
        printf("DATA packet: wrote %d bytes (%zu/%zu)\n",
               k, app->bytes_written, app->expected_size);
        fflush(stdout);
        return 0;
    }

    if (control == APP_END) {
        char received_name[256] = {0};
        size_t received_size = 0;
        if (parse_control_packet(payload, payload_len, APP_END,
                                 received_name, sizeof(received_name),
                                 &received_size) < 0) {
            fprintf(stderr, "Invalid END packet\n");
            return -1;
        }

        printf("END packet: file=%s size=%zu\n", received_name, received_size);
        printf("Receiver wrote %zu bytes to %s\n",
               app->bytes_written,
               app->output_path[0] ? app->output_path : "(unknown)");
        fflush(stdout);

        if (app->fp != NULL) {
            fclose(app->fp);
            app->fp = NULL;
        }

        if (app->expected_size != app->bytes_written) {
            fprintf(stderr,
                    "Warning: expected %zu bytes but wrote %zu bytes\n",
                    app->expected_size, app->bytes_written);
        }

        app->ended = TRUE;
        return 0;
    }

    fprintf(stderr, "Unknown application packet 0x%02X\n", control);
    return -1;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <SerialPort> [OutputDir]\nExample: %s /dev/ttyS1 .\n",
               argv[0], argv[0]);
        return 1;
    }

    const char *serialPortName = argv[1];
    const char *outputDir = (argc >= 3) ? argv[2] : ".";

    int fd = open(serialPortName, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror(serialPortName);
        return 1;
    }

    struct termios oldtio;
    if (configure_port(fd, &oldtio) < 0) {
        close(fd);
        return 1;
    }

    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
            perror("fcntl");
            tcsetattr(fd, TCSANOW, &oldtio);
            close(fd);
            return 1;
        }
    }

    printf("Receiver ready on %s\n", serialPortName);
    printf("Output directory: %s\n", outputDir);
    fflush(stdout);

    state_t state = START;
    conn_state_t connState = DISCONNECTED;
    app_state_t app;
    memset(&app, 0, sizeof(app));
    snprintf(app.output_dir, sizeof(app.output_dir), "%s", outputDir);

    unsigned char byte = 0;
    unsigned char A = 0;
    unsigned char C = 0;
    unsigned char data[MAX_DATA_SIZE];
    int data_index = 0;
    int expectedSeq = 0;

    while (1) {
        int res = read_byte(fd, &byte);
        if (res < 0)
            break;
        if (res == 0)
            continue;

        printf("RX byte = 0x%02X\n", byte);
        fflush(stdout);

        switch (state) {
            case START:
                if (byte == FLAG)
                    state = FLAG_RCV;
                break;

            case FLAG_RCV:
                if (byte == FLAG) {
                } else if (byte == A_TX_CMD) {
                    A = byte;
                    state = A_RCV;
                } else {
                    printf("Ignored after FLAG: 0x%02X\n", byte);
                    fflush(stdout);
                    state = START;
                }
                break;

            case A_RCV:
                if (byte == FLAG) {
                    state = FLAG_RCV;
                } else if (connState == DISCONNECTED && byte == C_SET) {
                    C = byte;
                    state = C_RCV;
                } else if (connState == CONNECTED &&
                           (byte == C_I0 || byte == C_I1 ||
                            byte == C_DISC || byte == C_SET)) {
                    C = byte;
                    state = C_RCV;
                } else if (connState == DISCONNECTING && byte == C_UA) {
                    C = byte;
                    state = C_RCV;
                } else {
                    printf("Unexpected C byte 0x%02X (connState=%d)\n",
                           byte, connState);
                    fflush(stdout);
                    state = START;
                }
                break;

            case C_RCV:
                if (byte == FLAG) {
                    state = FLAG_RCV;
                } else if (byte == (unsigned char)(A ^ C)) {
                    state = BCC1_OK;
                } else {
                    printf("BCC1 error: got 0x%02X expected 0x%02X\n",
                           byte, (unsigned char)(A ^ C));
                    fflush(stdout);
                    state = START;
                }
                break;

            case BCC1_OK:
                if (byte == FLAG) {
                    if (C == C_SET) {
                        printf("Valid SET received\n");
                        fflush(stdout);
                        if (send_supervision(fd, A_RX_REPLY, C_UA) < 0)
                            goto cleanup;
                        connState = CONNECTED;
                        printf("UA sent. Connection established.\n");
                        fflush(stdout);
                    } else if (C == C_DISC) {
                        printf("DISC received\n");
                        fflush(stdout);
                        if (send_supervision(fd, A_RX_REPLY, C_DISC) < 0)
                            goto cleanup;
                        connState = DISCONNECTING;
                        printf("DISC sent. Waiting for UA...\n");
                        fflush(stdout);
                    } else if (C == C_UA && connState == DISCONNECTING) {
                        printf("UA received. Disconnection complete.\n");
                        fflush(stdout);
                        goto cleanup;
                    } else {
                        printf("Unexpected short frame C=0x%02X\n", C);
                        fflush(stdout);
                    }
                    state = START;
                    data_index = 0;
                } else {
                    if (connState != CONNECTED) {
                        printf("Data before connection. Dropping.\n");
                        fflush(stdout);
                        state = START;
                        data_index = 0;
                    } else if (C == C_I0 || C == C_I1) {
                        data[0] = byte;
                        data_index = 1;
                        state = (byte == ESC) ? ESC_RCV : DATA_RCV;
                    } else {
                        printf("Unexpected data after BCC1 for C=0x%02X\n", C);
                        fflush(stdout);
                        state = START;
                        data_index = 0;
                    }
                }
                break;

            case DATA_RCV:
                if (byte == FLAG) {
                    if (data_index < 1) {
                        printf("Invalid I-frame: missing BCC2\n");
                        fflush(stdout);
                        state = START;
                        data_index = 0;
                        break;
                    }

                    unsigned char bcc2_read = data[data_index - 1];
                    unsigned char bcc2_calc = 0x00;
                    for (int i = 0; i < data_index - 1; i++)
                        bcc2_calc ^= data[i];

                    int frameSeq = (C == C_I1) ? 1 : 0;
                    int is_new = (frameSeq == expectedSeq);

                    printf("I-frame seq=%d payload_len=%d BCC2 read=0x%02X calc=0x%02X\n",
                           frameSeq, data_index - 1, bcc2_read, bcc2_calc);
                    fflush(stdout);

                    if (bcc2_read == bcc2_calc) {
                        if (is_new) {
                            if (process_app_packet(data, data_index - 1, &app) < 0)
                                goto cleanup;
                            expectedSeq = 1 - expectedSeq;
                            unsigned char rrC = (expectedSeq == 1) ? C_RR1 : C_RR0;
                            printf("Accepted I(Ns=%d). Sending %s.\n",
                                   frameSeq, (rrC == C_RR1) ? "RR1" : "RR0");
                            fflush(stdout);
                            if (send_supervision(fd, A_RX_REPLY, rrC) < 0)
                                goto cleanup;
                        } else {
                            unsigned char rrC = (expectedSeq == 1) ? C_RR1 : C_RR0;
                            printf("Duplicate I(Ns=%d). Re-sending %s.\n",
                                   frameSeq, (rrC == C_RR1) ? "RR1" : "RR0");
                            fflush(stdout);
                            if (send_supervision(fd, A_RX_REPLY, rrC) < 0)
                                goto cleanup;
                        }
                    } else {
                        if (is_new) {
                            unsigned char rejC = (expectedSeq == 0) ? C_REJ0 : C_REJ1;
                            printf("BCC2 error on expected I(Ns=%d). Sending %s.\n",
                                   frameSeq, (rejC == C_REJ0) ? "REJ0" : "REJ1");
                            fflush(stdout);
                            if (send_supervision(fd, A_RX_REPLY, rejC) < 0)
                                goto cleanup;
                        } else {
                            unsigned char rrC = (expectedSeq == 1) ? C_RR1 : C_RR0;
                            printf("BCC2 error on duplicate I(Ns=%d). Re-sending %s.\n",
                                   frameSeq, (rrC == C_RR1) ? "RR1" : "RR0");
                            fflush(stdout);
                            if (send_supervision(fd, A_RX_REPLY, rrC) < 0)
                                goto cleanup;
                        }
                    }

                    state = START;
                    data_index = 0;
                } else {
                    if (data_index >= MAX_DATA_SIZE) {
                        printf("Buffer overflow. Dropping frame.\n");
                        fflush(stdout);
                        state = START;
                        data_index = 0;
                    } else {
                        data[data_index++] = byte;
                        if (byte == ESC)
                            state = ESC_RCV;
                    }
                }
                break;

            case ESC_RCV:
                if (byte == FLAG) {
                    printf("Invalid escape before FLAG. Dropping.\n");
                    fflush(stdout);
                    state = START;
                    data_index = 0;
                } else {
                    if (data_index <= 0 || data_index > MAX_DATA_SIZE) {
                        state = START;
                        data_index = 0;
                    } else {
                        data[data_index - 1] = (unsigned char)(byte ^ ESC_XOR);
                        state = DATA_RCV;
                    }
                }
                break;
        }
    }

cleanup:
    if (app.fp != NULL)
        fclose(app.fp);
    sleep(1);
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");
    close(fd);
    return 0;
}
