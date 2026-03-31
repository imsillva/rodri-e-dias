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

#define FLAG   0x7E
#define A_TX   0x03
#define A_RX   0x01
#define C_SET  0x03
#define C_UA   0x07
#define C_I0   0x00
#define C_I1   0x40
#define C_RR0  0x05
#define C_RR1  0x85
#define C_REJ0 0x01
#define C_REJ1 0x81

#define MAX_RETRIES 5
#define TIMEOUT_SECS 3
#define MAX_PAYLOAD_SIZE 256

typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    BCC_OK,
    STOP_STATE
} state_t;

static volatile int alarmEnabled = FALSE;
static volatile int alarmCount = 0;

static void alarmHandler(int signal)
{
    (void)signal;
    alarmEnabled = FALSE;
    alarmCount++;
    printf("Alarm #%d received\n", alarmCount);
}

static int send_supervision(int fd, unsigned char A, unsigned char C)
{
    unsigned char frame[5];
    frame[0] = FLAG;
    frame[1] = A;
    frame[2] = C;
    frame[3] = A ^ C;
    frame[4] = FLAG;

    int res = write(fd, frame, sizeof(frame));
    if (res < 0)
        perror("write");
    return res;
}

static int send_iframe(int fd, const unsigned char *data, int data_len, int seq)
{
    if (data_len < 0 || data_len > MAX_PAYLOAD_SIZE)
        return -1;

    unsigned char frame[4 + MAX_PAYLOAD_SIZE + 2];
    unsigned char C = seq ? C_I1 : C_I0;
    unsigned char bcc2 = 0x00;

    frame[0] = FLAG;
    frame[1] = A_TX;
    frame[2] = C;
    frame[3] = frame[1] ^ frame[2];

    for (int i = 0; i < data_len; i++) {
        frame[4 + i] = data[i];
        bcc2 ^= data[i];
    }

    frame[4 + data_len] = bcc2;
    frame[5 + data_len] = FLAG;

    int frame_len = 6 + data_len - 0;
    int res = write(fd, frame, frame_len);
    if (res < 0)
        perror("write");

    return res;
}

static int read_supervision_frame(int fd, unsigned char expectedA,
                                  const unsigned char *acceptedC,
                                  int acceptedCount,
                                  unsigned char *receivedC)
{
    state_t state = START;
    unsigned char byte = 0;
    unsigned char A = 0;
    unsigned char C = 0;

    while (1) {
        int res = read(fd, &byte, 1);

        if (res < 0) {
            if (errno == EINTR)
                return 0; /* timeout */
            perror("read");
            return -1;
        }

        if (res == 0)
            continue;

        printf("RX ctrl byte = 0x%02X\n", byte);

        switch (state) {
            case START:
                if (byte == FLAG)
                    state = FLAG_RCV;
                break;

            case FLAG_RCV:
                if (byte == FLAG) {
                    /* keep sync */
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
                } else if (byte == (A ^ C)) {
                    state = BCC_OK;
                } else {
                    state = START;
                }
                break;

            case BCC_OK:
                if (byte == FLAG) {
                    if (receivedC)
                        *receivedC = C;
                    return 1;
                }
                state = START;
                break;

            case STOP_STATE:
                return -1;
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Incorrect program usage\n"
               "Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n",
               argv[0], argv[0]);
        exit(1);
    }

    const char *serialPortName = argv[1];

    int fd = open(serialPortName, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror(serialPortName);
        exit(-1);
    }

    struct termios oldtio;
    struct termios newtio;

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
    newtio.c_cc[VMIN] = 1;

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    printf("New termios structure set\n");

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = &alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    printf("Alarm configured\n");

    /* 1) Establish connection: send SET, wait UA */
    unsigned char uaC = 0;
    unsigned char acceptedUA[] = {C_UA};
    int connected = FALSE;

    while (!connected && alarmCount < MAX_RETRIES) {
        if (!alarmEnabled) {
            printf("Sending SET...\n");
            send_supervision(fd, A_TX, C_SET);
            alarm(TIMEOUT_SECS);
            alarmEnabled = TRUE;
        }

        int res = read_supervision_frame(fd, A_RX, acceptedUA, 1, &uaC);
        if (res == 1) {
            alarm(0);
            alarmEnabled = FALSE;
            connected = TRUE;
            printf("UA received. Connection established.\n");
        } else if (res < 0) {
            break;
        }
    }

    if (!connected) {
        printf("Failed to establish connection\n");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return 1;
    }

    /* 2) Send one I-frame with seq = 0 and wait RR1 / REJ0 */
    unsigned char payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    unsigned char acceptedAck[] = {C_RR1, C_REJ0};
    unsigned char ackC = 0;
    int done = FALSE;
    alarmCount = 0;
    alarmEnabled = FALSE;

    while (!done && alarmCount < MAX_RETRIES) {
        if (!alarmEnabled) {
            printf("Sending I0 frame...\n");
            send_iframe(fd, payload, (int)sizeof(payload), 0);
            alarm(TIMEOUT_SECS);
            alarmEnabled = TRUE;
        }

        int res = read_supervision_frame(fd, A_RX, acceptedAck, 2, &ackC);
        if (res == 1) {
            alarm(0);
            alarmEnabled = FALSE;

            if (ackC == C_RR1) {
                printf("RR1 received. Frame accepted.\n");
                done = TRUE;
            } else if (ackC == C_REJ0) {
                printf("REJ0 received. Retransmitting...\n");
                alarmEnabled = FALSE;
            }
        } else if (res < 0) {
            break;
        }
    }

    if (!done) {
        printf("Failed to send I-frame successfully\n");
    }

    sleep(1);

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    close(fd);
    return 0;
}
