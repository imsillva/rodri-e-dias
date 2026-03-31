#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define BAUDRATE B38400
#define _POSIX_SOURCE 1

#define FALSE 0
#define TRUE 1

#define MAX_DATA_SIZE 256
#define SU_FRAME_SIZE 5

#define FLAG   0x7E
#define A_TX   0x03                  // comandos do transmissor
#define A_RX   0x01                  // respostas do recetor
// Se o teu emissor estiver à espera de 0x03 nas respostas, troca para 0x03.

#define C_SET  0x03
#define C_UA   0x07
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
    BCC1_RCV,
    DATA_RCV
} state_t;

typedef enum {
    DISCONNECTED,
    CONNECTED
} conn_state_t;

static void send_supervision(int fd, unsigned char A, unsigned char C) {
    unsigned char frame[SU_FRAME_SIZE];
    frame[0] = FLAG;
    frame[1] = A;
    frame[2] = C;
    frame[3] = A ^ C;
    frame[4] = FLAG;

    if (write(fd, frame, SU_FRAME_SIZE) < 0) {
        perror("write");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n", argv[0], argv[0]);
        exit(1);
    }

    const char *serialPortName = argv[1];

    int fd = open(serialPortName, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror(serialPortName);
        exit(-1);
    }

    struct termios oldtio, newtio;
    if (tcgetattr(fd, &oldtio) == -1) {
        perror("tcgetattr");
        exit(-1);
    }

    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN]  = 1;   // importante: 1 byte de cada vez

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    printf("New termios structure set\n");

    state_t currentState = START;
    conn_state_t connState = DISCONNECTED;

    unsigned char byte = 0;
    unsigned char A = 0, C = 0;
    unsigned char data[MAX_DATA_SIZE];
    int data_index = 0;
    int expectedSeq = 0;

    while (1) {
        int bytes_read = read(fd, &byte, 1);
        if (bytes_read < 0) {
            perror("read");
            break;
        }
        if (bytes_read == 0) continue;

        printf("byte = 0x%02X\n", byte);

        switch (currentState) {
            case START:
                if (byte == FLAG) {
                    currentState = FLAG_RCV;
                }
                break;

            case FLAG_RCV:
                if (byte == FLAG) {
                    // mantém
                } else if (byte == A_TX) {
                    A = byte;
                    currentState = A_RCV;
                } else {
                    currentState = START;
                }
                break;

            case A_RCV:
                if (byte == FLAG) {
                    currentState = FLAG_RCV;
                } else if (connState == DISCONNECTED && byte == C_SET) {
                    C = byte;
                    currentState = C_RCV;
                } else if (connState == CONNECTED &&
                           (byte == C_I0 || byte == C_I1)) {
                    C = byte;
                    currentState = C_RCV;
                } else {
                    currentState = START;
                }
                break;

            case C_RCV:
                if (byte == FLAG) {
                    currentState = FLAG_RCV;
                } else if (byte == (A ^ C)) {
                    currentState = BCC1_RCV;
                } else {
                    currentState = START;
                }
                break;

            case BCC1_RCV:
                if (byte == FLAG) {
                    // trama sem campo de dados
                    if (connState == DISCONNECTED && C == C_SET) {
                        printf("SET recebido com sucesso\n");
                        send_supervision(fd, A_RX, C_UA);
                        printf("UA enviada\n");
                        connState = CONNECTED;
                        printf("Ligação estabelecida. À espera de I-frames...\n");
                    }
                    currentState = START;
                    data_index = 0;
                } else {
                    // primeiro byte de dados
                    data[0] = byte;
                    data_index = 1;
                    currentState = DATA_RCV;
                }
                break;

            case DATA_RCV:
                if (byte == FLAG) {
                    if (data_index < 1) {
                        printf("Erro: trama demasiado curta\n");
                        currentState = START;
                        data_index = 0;
                        break;
                    }

                    unsigned char bcc2_read = data[data_index - 1];
                    unsigned char bcc2_calc = 0x00;

                    for (int i = 0; i < data_index - 1; i++) {
                        printf("data[%d] = 0x%02X\n", i, data[i]);
                        bcc2_calc ^= data[i];
                    }

                    printf("BCC2 lido      = 0x%02X\n", bcc2_read);
                    printf("BCC2 calculado = 0x%02X\n", bcc2_calc);

                    int frameSeq = (C == C_I0) ? 0 : 1;

                    if (bcc2_read == bcc2_calc) {
                        if (frameSeq == expectedSeq) {
                            printf("I-frame %d válida\n", frameSeq);
                            expectedSeq = 1 - expectedSeq;

                            if (expectedSeq == 0)
                                send_supervision(fd, A_RX, C_RR0);
                            else
                                send_supervision(fd, A_RX, C_RR1);
                        } else {
                            printf("I-frame duplicada (seq=%d, esperado=%d)\n",
                                   frameSeq, expectedSeq);

                            if (expectedSeq == 0)
                                send_supervision(fd, A_RX, C_RR0);
                            else
                                send_supervision(fd, A_RX, C_RR1);
                        }
                    } else {
                        printf("Erro no BCC2 — trama rejeitada\n");

                        if (expectedSeq == 0)
                            send_supervision(fd, A_RX, C_REJ0);
                        else
                            send_supervision(fd, A_RX, C_REJ1);
                    }

                    currentState = START;
                    data_index = 0;
                } else {
                    if (data_index >= MAX_DATA_SIZE) {
                        printf("Overflow no buffer\n");
                        currentState = START;
                        data_index = 0;
                    } else {
                        data[data_index++] = byte;
                    }
                }
                break;
        }
    }

    sleep(1);

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    close(fd);
    return 0;
}
