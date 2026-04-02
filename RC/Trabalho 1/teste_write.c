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

#define MAX_RETRIES 5
#define TIMEOUT_SECS 3
#define MAX_PAYLOAD_SIZE 256
#define MAX_FRAME_SIZE (4 + 2 * (MAX_PAYLOAD_SIZE + 1) + 2)

typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    BCC_OK
} state_t;

static volatile sig_atomic_t alarmEnabled = FALSE;
static volatile sig_atomic_t alarmCount = 0;

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
            fprintf(stderr, "write returned 0 unexpectedly\n");
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

/* FIX: trata EAGAIN (porto em modo não-bloqueante) e EINTR (sinal) */
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
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 1;
    newtio.c_cc[VMIN] = 0;

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

static int send_supervision(int fd, unsigned char A, unsigned char C)
{
    unsigned char frame[5];
    frame[0] = FLAG;
    frame[1] = A;
    frame[2] = C;
    frame[3] = A ^ C;
    frame[4] = FLAG;

    printf("TX S-frame: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
           frame[0], frame[1], frame[2], frame[3], frame[4]);
    fflush(stdout);

    return write_all(fd, frame, sizeof(frame));
}

static int build_iframe(unsigned char *frame,
                        const unsigned char *data,
                        int data_len,
                        int seq)
{
    if (data_len < 0 || data_len > MAX_PAYLOAD_SIZE)
        return -1;

    unsigned char C = seq ? C_I1 : C_I0;
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

    printf("TX I-frame bytes:");
    for (int i = 0; i < frame_len; i++)
        printf(" 0x%02X", frame[i]);
    printf("\n");
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
    unsigned char byte = 0;
    unsigned char A = 0;
    unsigned char C = 0;

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
                    /* múltiplos FLAGS consecutivos são válidos */
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
                if (byte == FLAG) {
                    state = FLAG_RCV;
                } else if (byte == (unsigned char)(A ^ C)) {
                    state = BCC_OK;
                } else {
                    state = START;
                }
                break;

            case BCC_OK:
                if (byte == FLAG) {
                    if (receivedC != NULL)
                        *receivedC = C;
                    return 1;
                }
                state = START;
                break;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS0\n", argv[0], argv[0]);
        return 1;
    }

    const char *serialPortName = argv[1];

    /* FIX: abrir com O_NONBLOCK para evitar bloqueio na espera de DCD */
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

    /* FIX: limpar O_NONBLOCK após configurar a porta;
     * o VTIME=1 VMIN=0 do termios controla agora o timeout das leituras */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
            perror("fcntl");
            tcsetattr(fd, TCSANOW, &oldtio);
            close(fd);
            return 1;
        }
    }

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;   /* sem SA_RESTART para que read() retorne EINTR */
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return 1;
    }

    printf("Transmitter ready on %s\n", serialPortName);
    fflush(stdout);

    unsigned char acceptedUA[] = {C_UA};
    unsigned char receivedC = 0;
    int connected = FALSE;

    while (!connected && alarmCount < MAX_RETRIES) {
        if (!alarmEnabled) {
            if (send_supervision(fd, A_TX_CMD, C_SET) < 0)
                goto cleanup_error;
            /* FIX: definir alarmEnabled ANTES de armar o alarme */
            alarmEnabled = TRUE;
            alarm(TIMEOUT_SECS);
        }

        int res = read_supervision_frame(fd, A_RX_REPLY, acceptedUA, 1, &receivedC);
        if (res < 0)
            goto cleanup_error;
        if (res == 1) {
            alarm(0);
            alarmEnabled = FALSE;
            connected = TRUE;
            printf("UA received. Connection established.\n");
            fflush(stdout);
        }
    }

    if (!connected) {
        printf("Failed to establish connection\n");
        fflush(stdout);
        goto cleanup_error;
    }

    unsigned char payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    int seq = 0;
    unsigned char acceptedAck[] = {C_RR1, C_REJ0};
    int frameDone = FALSE;

    alarmCount = 0;
    alarmEnabled = FALSE;

    while (!frameDone && alarmCount < MAX_RETRIES) {
        if (!alarmEnabled) {
            if (send_iframe(fd, payload, (int)sizeof(payload), seq) < 0)
                goto cleanup_error;
            /* FIX: definir alarmEnabled ANTES de armar o alarme */
            alarmEnabled = TRUE;
            alarm(TIMEOUT_SECS);
        }

        int res = read_supervision_frame(fd, A_RX_REPLY, acceptedAck, 2, &receivedC);
        if (res < 0)
            goto cleanup_error;
        if (res == 1) {
            alarm(0);
            alarmEnabled = FALSE;

            if (receivedC == C_RR1) {
                printf("RR1 received. I0 accepted.\n");
                fflush(stdout);
                frameDone = TRUE;
            } else if (receivedC == C_REJ0) {
                printf("REJ0 received. Retransmitting I0.\n");
                fflush(stdout);
            }
        }
    }

    if (!frameDone) {
        printf("Failed to send I-frame successfully\n");
        fflush(stdout);
        goto cleanup_error;
    }

    sleep(1);
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");
    close(fd);
    return 0;

cleanup_error:
    sleep(1);
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");
    close(fd);
    return 1;
}
