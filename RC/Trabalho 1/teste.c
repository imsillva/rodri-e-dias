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
    CONNECTED
} conn_state_t;

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

/* FIX: trata EAGAIN (porto não-bloqueante) e EINTR (sinal) */
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
    unsigned char frame[SU_FRAME_SIZE];
    frame[0] = FLAG;
    frame[1] = A;
    frame[2] = C;
    frame[3] = A ^ C;
    frame[4] = FLAG;

    printf("TX S-frame: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
           frame[0], frame[1], frame[2], frame[3], frame[4]);
    fflush(stdout);

    return write_all(fd, frame, SU_FRAME_SIZE);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n", argv[0], argv[0]);
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

    printf("Receiver ready on %s\n", serialPortName);
    fflush(stdout);

    state_t state = START;
    conn_state_t connState = DISCONNECTED;

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
                    /* múltiplos FLAGS consecutivos são válidos */
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
                           (byte == C_SET || byte == C_I0 ||
                            byte == C_I1 || byte == C_DISC)) {
                    C = byte;
                    state = C_RCV;
                } else {
                    printf("Unexpected control byte 0x%02X in A_RCV\n", byte);
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
                    } else {
                        printf("Unexpected short frame with C=0x%02X\n", C);
                        fflush(stdout);
                    }
                    state = START;
                    data_index = 0;
                } else {
                    if (connState != CONNECTED) {
                        printf("Data before connection. Dropping frame.\n");
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
                    printf("I-frame seq=%d, payload_len=%d, "
                           "BCC2 read=0x%02X calc=0x%02X\n",
                           frameSeq, data_index - 1, bcc2_read, bcc2_calc);
                    fflush(stdout);

                    if (bcc2_read == bcc2_calc) {
                        if (frameSeq == expectedSeq) {
                            expectedSeq = 1 - expectedSeq;
                            /* envia RRn onde n é o próximo seq esperado */
                            unsigned char rrC = (expectedSeq == 1) ? C_RR1 : C_RR0;
                            if (send_supervision(fd, A_RX_REPLY, rrC) < 0)
                                goto cleanup;
                        } else {
                            /* duplicado: reenviar o último ACK */
                            unsigned char rrC = (expectedSeq == 1) ? C_RR1 : C_RR0;
                            if (send_supervision(fd, A_RX_REPLY, rrC) < 0)
                                goto cleanup;
                        }
                    } else {
                        /* BCC2 errado */
                        if (frameSeq == expectedSeq) {
                            /* rejeitar a trama esperada */
                            unsigned char rejC = (expectedSeq == 0) ? C_REJ0 : C_REJ1;
                            if (send_supervision(fd, A_RX_REPLY, rejC) < 0)
                                goto cleanup;
                        } else {
                            /* duplicado com erro: repetir ACK anterior */
                            unsigned char rrC = (expectedSeq == 1) ? C_RR1 : C_RR0;
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
                    printf("Invalid escape sequence before FLAG. Dropping frame.\n");
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
    sleep(1);
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");
    close(fd);
    return 0;
}
