#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

#define MAX_RETRIES      5
#define TIMEOUT_SECS     3
#define MAX_PAYLOAD_SIZE 256
#define MAX_FRAME_SIZE   (4 + 2 * (MAX_PAYLOAD_SIZE + 1) + 2)
#define APP_DATA_CHUNK   (MAX_PAYLOAD_SIZE - 3)

typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    BCC_OK
} state_t;

static volatile sig_atomic_t alarmEnabled = FALSE;
static volatile sig_atomic_t alarmCount   = 0;

static void alarmHandler(int signo)
{
    (void)signo;
    alarmEnabled = FALSE;
    alarmCount++;
    printf("Alarm #%d received\n", (int)alarmCount);
    fflush(stdout);
}

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
    unsigned char frame[5] = { FLAG, A, C, (unsigned char)(A ^ C), FLAG };
    printf("TX S-frame: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
           frame[0], frame[1], frame[2], frame[3], frame[4]);
    fflush(stdout);
    return write_all(fd, frame, sizeof(frame));
}

static int build_iframe(unsigned char *frame,
                        const unsigned char *data, int data_len, int seq)
{
    if (data_len < 0 || data_len > MAX_PAYLOAD_SIZE)
        return -1;

    unsigned char C    = seq ? C_I1 : C_I0;
    unsigned char bcc2 = 0x00;
    int idx = 0;

    frame[idx++] = FLAG;
    frame[idx++] = A_TX_CMD;
    frame[idx++] = C;
    frame[idx++] = (unsigned char)(A_TX_CMD ^ C);

    for (int i = 0; i < data_len; i++)
        bcc2 ^= data[i];

    for (int i = 0; i < data_len; i++) {
        if (data[i] == FLAG || data[i] == ESC) {
            frame[idx++] = ESC;
            frame[idx++] = (unsigned char)(data[i] ^ ESC_XOR);
        } else {
            frame[idx++] = data[i];
        }
    }

    if (bcc2 == FLAG || bcc2 == ESC) {
        frame[idx++] = ESC;
        frame[idx++] = (unsigned char)(bcc2 ^ ESC_XOR);
    } else {
        frame[idx++] = bcc2;
    }

    frame[idx++] = FLAG;
    return idx;
}

static int send_iframe(int fd, const unsigned char *data, int data_len, int seq)
{
    unsigned char frame[MAX_FRAME_SIZE];
    int frame_len = build_iframe(frame, data, data_len, seq);
    if (frame_len < 0)
        return -1;

    printf("TX I-frame (seq=%d, payload_len=%d)\n", seq, data_len);
    fflush(stdout);
    return write_all(fd, frame, (size_t)frame_len);
}

static int read_supervision_frame(int fd,
                                  unsigned char expectedA,
                                  const unsigned char *acceptedC,
                                  int acceptedCount,
                                  unsigned char *receivedC)
{
    state_t state = START;
    unsigned char byte = 0, A = 0, C = 0;

    while (alarmEnabled) {
        int res = read_byte(fd, &byte);
        if (res < 0)
            return -1;
        if (res == 0)
            continue;

        printf("RX ctrl byte = 0x%02X\n", byte);
        fflush(stdout);

        switch (state) {
            case START:
                if (byte == FLAG)
                    state = FLAG_RCV;
                break;

            case FLAG_RCV:
                if (byte == FLAG) {
                } else if (byte == expectedA) {
                    A = byte;
                    state = A_RCV;
                } else {
                    state = START;
                }
                break;

            case A_RCV: {
                if (byte == FLAG) {
                    state = FLAG_RCV;
                    break;
                }
                int ok = 0;
                for (int i = 0; i < acceptedCount; i++) {
                    if (byte == acceptedC[i]) {
                        ok = 1;
                        break;
                    }
                }
                if (ok) {
                    C = byte;
                    state = C_RCV;
                } else {
                    state = START;
                }
                break;
            }

            case C_RCV:
                if (byte == FLAG)
                    state = FLAG_RCV;
                else if (byte == (unsigned char)(A ^ C))
                    state = BCC_OK;
                else
                    state = START;
                break;

            case BCC_OK:
                if (byte == FLAG) {
                    if (receivedC)
                        *receivedC = C;
                    return 1;
                }
                state = START;
                break;
        }
    }

    return 0;
}

static const char *get_basename_only(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash != NULL && slash[1] != '\0')
        return slash + 1;
    return path;
}

static int encode_size_tlv(unsigned char *dst, size_t file_size)
{
    unsigned char tmp[sizeof(size_t)];
    int n = 0;
    size_t value = file_size;

    do {
        tmp[n++] = (unsigned char)(value & 0xFFu);
        value >>= 8;
    } while (value != 0 && n < (int)sizeof(tmp));

    dst[0] = APP_T_SIZE;
    dst[1] = (unsigned char)n;
    for (int i = 0; i < n; i++)
        dst[2 + i] = tmp[n - 1 - i];

    return 2 + n;
}

static int build_control_packet(unsigned char *packet,
                                unsigned char control,
                                const char *filename,
                                size_t file_size)
{
    const char *base = get_basename_only(filename);
    size_t name_len = strlen(base);
    if (name_len > 255)
        return -1;

    int idx = 0;
    packet[idx++] = control;
    idx += encode_size_tlv(&packet[idx], file_size);
    packet[idx++] = APP_T_NAME;
    packet[idx++] = (unsigned char)name_len;
    memcpy(&packet[idx], base, name_len);
    idx += (int)name_len;

    if (idx > MAX_PAYLOAD_SIZE)
        return -1;
    return idx;
}

static int build_data_packet(unsigned char *packet,
                             const unsigned char *data,
                             size_t data_len)
{
    if (data_len > APP_DATA_CHUNK)
        return -1;

    packet[0] = APP_DATA;
    packet[1] = (unsigned char)((data_len >> 8) & 0xFFu);
    packet[2] = (unsigned char)(data_len & 0xFFu);
    memcpy(&packet[3], data, data_len);
    return (int)(3 + data_len);
}

static int send_packet_stopwait(int fd,
                                const unsigned char *packet,
                                int packet_len,
                                int *seq)
{
    unsigned char positiveAck = (*seq == 0) ? C_RR1 : C_RR0;
    unsigned char negativeAck = (*seq == 0) ? C_REJ0 : C_REJ1;
    unsigned char accepted[2] = { positiveAck, negativeAck };
    unsigned char receivedC = 0;

    alarmCount = 0;
    alarmEnabled = FALSE;

    while (alarmCount < MAX_RETRIES) {
        if (!alarmEnabled) {
            if (send_iframe(fd, packet, packet_len, *seq) < 0)
                return -1;
            alarmEnabled = TRUE;
            alarm(TIMEOUT_SECS);
        }

        int res = read_supervision_frame(fd, A_RX_REPLY, accepted, 2, &receivedC);
        if (res < 0)
            return -1;
        if (res == 1) {
            alarm(0);
            alarmEnabled = FALSE;
            if (receivedC == positiveAck) {
                printf("Positive ACK received for seq=%d\n", *seq);
                fflush(stdout);
                *seq = 1 - *seq;
                return packet_len;
            }
            if (receivedC == negativeAck) {
                printf("Negative ACK received for seq=%d. Retransmitting...\n", *seq);
                fflush(stdout);
                alarmEnabled = FALSE;
            }
        }
    }

    return -1;
}

static int llopen_tx(int fd)
{
    unsigned char acceptedUA[] = { C_UA };
    unsigned char receivedC = 0;
    int connected = FALSE;

    alarmCount = 0;
    alarmEnabled = FALSE;

    while (!connected && alarmCount < MAX_RETRIES) {
        if (!alarmEnabled) {
            if (send_supervision(fd, A_TX_CMD, C_SET) < 0)
                return -1;
            alarmEnabled = TRUE;
            alarm(TIMEOUT_SECS);
        }

        int res = read_supervision_frame(fd, A_RX_REPLY, acceptedUA, 1, &receivedC);
        if (res < 0)
            return -1;
        if (res == 1) {
            alarm(0);
            alarmEnabled = FALSE;
            connected = TRUE;
            printf("UA received. Connection established.\n");
            fflush(stdout);
        }
    }

    return connected ? 0 : -1;
}

static int llclose_tx(int fd)
{
    unsigned char acceptedDISC[] = { C_DISC };
    unsigned char receivedC = 0;
    int discDone = FALSE;

    alarmCount = 0;
    alarmEnabled = FALSE;

    while (!discDone && alarmCount < MAX_RETRIES) {
        if (!alarmEnabled) {
            if (send_supervision(fd, A_TX_CMD, C_DISC) < 0)
                return -1;
            alarmEnabled = TRUE;
            alarm(TIMEOUT_SECS);
        }

        int res = read_supervision_frame(fd, A_RX_REPLY, acceptedDISC, 1, &receivedC);
        if (res < 0)
            return -1;
        if (res == 1) {
            alarm(0);
            alarmEnabled = FALSE;
            discDone = TRUE;
            printf("DISC received from receiver.\n");
            fflush(stdout);
        }
    }

    if (!discDone)
        return -1;

    if (send_supervision(fd, A_TX_CMD, C_UA) < 0)
        return -1;

    printf("UA sent. Disconnection complete.\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("Usage: %s <SerialPort> <ImageFile>\nExample: %s /dev/ttyS0 image.jpg\n",
               argv[0], argv[0]);
        return 1;
    }

    const char *serialPortName = argv[1];
    const char *imagePath = argv[2];

    FILE *fp = fopen(imagePath, "rb");
    if (fp == NULL) {
        perror(imagePath);
        return 1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(fp);
        return 1;
    }
    long file_size_long = ftell(fp);
    if (file_size_long < 0) {
        perror("ftell");
        fclose(fp);
        return 1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        perror("fseek");
        fclose(fp);
        return 1;
    }
    size_t file_size = (size_t)file_size_long;

    int fd = open(serialPortName, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror(serialPortName);
        fclose(fp);
        return 1;
    }

    struct termios oldtio;
    if (configure_port(fd, &oldtio) < 0) {
        close(fd);
        fclose(fp);
        return 1;
    }

    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
            perror("fcntl");
            tcsetattr(fd, TCSANOW, &oldtio);
            close(fd);
            fclose(fp);
            return 1;
        }
    }

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        fclose(fp);
        return 1;
    }

    printf("Transmitter ready on %s\n", serialPortName);
    printf("Sending file: %s (%zu bytes)\n", imagePath, file_size);
    fflush(stdout);

    int seq = 0;
    unsigned char packet[MAX_PAYLOAD_SIZE];

    if (llopen_tx(fd) < 0) {
        fprintf(stderr, "Failed to establish connection\n");
        goto cleanup_error;
    }

    int start_len = build_control_packet(packet, APP_START, imagePath, file_size);
    if (start_len < 0) {
        fprintf(stderr, "START packet too large\n");
        goto cleanup_error;
    }
    if (send_packet_stopwait(fd, packet, start_len, &seq) < 0) {
        fprintf(stderr, "Failed to send START packet\n");
        goto cleanup_error;
    }

    unsigned char chunk[APP_DATA_CHUNK];
    size_t total_sent = 0;
    while (1) {
        size_t n = fread(chunk, 1, sizeof(chunk), fp);
        if (n > 0) {
            int data_len = build_data_packet(packet, chunk, n);
            if (data_len < 0) {
                fprintf(stderr, "DATA packet build error\n");
                goto cleanup_error;
            }
            if (send_packet_stopwait(fd, packet, data_len, &seq) < 0) {
                fprintf(stderr, "Failed to send DATA packet\n");
                goto cleanup_error;
            }
            total_sent += n;
            printf("Progress: %zu/%zu bytes sent\n", total_sent, file_size);
            fflush(stdout);
        }

        if (n < sizeof(chunk)) {
            if (ferror(fp)) {
                perror("fread");
                goto cleanup_error;
            }
            break;
        }
    }

    int end_len = build_control_packet(packet, APP_END, imagePath, file_size);
    if (end_len < 0) {
        fprintf(stderr, "END packet too large\n");
        goto cleanup_error;
    }
    if (send_packet_stopwait(fd, packet, end_len, &seq) < 0) {
        fprintf(stderr, "Failed to send END packet\n");
        goto cleanup_error;
    }

    if (llclose_tx(fd) < 0) {
        fprintf(stderr, "Failed to close connection cleanly\n");
        goto cleanup_error;
    }

    fclose(fp);
    sleep(1);
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");
    close(fd);
    return 0;

cleanup_error:
    fclose(fp);
    sleep(1);
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");
    close(fd);
    return 1;
}
