#define _POSIX_C_SOURCE 200809L
// Write to serial port in non-canonical mode
// Adapted from the non-canonical example and from the link layer llwrite logic.

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

// Baudrate settings are defined in <asm/termbits.h>, which is
// included by <termios.h>
#define BAUDRATE B38400

#define FALSE 0
#define TRUE 1

#define MAX_PAYLOAD_SIZE 1000
#define MAX_FRAME_SIZE (2 * MAX_PAYLOAD_SIZE + 6)
#define MAX_RETRANSMISSIONS 3
#define TIMEOUT_SECONDS 3

#define FLAG 0x7e
#define A_TX 0x03
#define A_RX 0x01
#define ESC 0x7d

#define CONTROL_REJ(Nr) (0xaa | (Nr))
#define CONTROL_RR(Nr)  (0x54 | (Nr))
#define CONTROL_N(Ns)   ((Ns) << 7)

typedef enum
{
    START_STATE,
    FIRST_FLAG_STATE,
    ADDRESS_STATE,
    CONTROL_STATE,
    BCC_STATE,
    FINAL_STATE
} State;

volatile sig_atomic_t timeoutFlag = FALSE;

static void alarmHandler(int signal)
{
    (void)signal;
    timeoutFlag = TRUE;
}

static int configureNonCanonical(int fd, struct termios *oldtio)
{
    struct termios newtio;

    if (tcgetattr(fd, oldtio) == -1)
        return -1;

    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;

    // Polling-style read: lets us implement timeouts/retransmissions in user code.
    newtio.c_cc[VTIME] = 1; // 0.1 s
    newtio.c_cc[VMIN] = 0;

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &newtio) == -1)
        return -1;

    return 0;
}

static int writeAll(int fd, const unsigned char *buf, int len)
{
    int total = 0;

    while (total < len)
    {
        int written = write(fd, buf + total, len - total);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += written;
    }

    return total;
}

static int buildInformationFrame(const unsigned char *payload, int payloadSize,
                                 unsigned char sequenceNumber,
                                 unsigned char *frame)
{
    unsigned char bcc2 = 0;
    int frameLen = 0;

    frame[frameLen++] = FLAG;
    frame[frameLen++] = A_TX;
    frame[frameLen++] = CONTROL_N(sequenceNumber);
    frame[frameLen++] = A_TX ^ CONTROL_N(sequenceNumber);

    for (int i = 0; i < payloadSize; i++)
    {
        bcc2 ^= payload[i];

        if (payload[i] == FLAG || payload[i] == ESC)
        {
            frame[frameLen++] = ESC;
            frame[frameLen++] = payload[i] ^ 0x20;
        }
        else
        {
            frame[frameLen++] = payload[i];
        }
    }

    if (bcc2 == FLAG || bcc2 == ESC)
    {
        frame[frameLen++] = ESC;
        frame[frameLen++] = bcc2 ^ 0x20;
    }
    else
    {
        frame[frameLen++] = bcc2;
    }

    frame[frameLen++] = FLAG;
    return frameLen;
}

static int readSupervisionFrame(int fd, unsigned char *control)
{
    unsigned char byte = 0;
    State state = START_STATE;

    while (!timeoutFlag && state != FINAL_STATE)
    {
        int bytes = read(fd, &byte, 1);
        if (bytes < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (bytes == 0)
            continue;

        switch (state)
        {
        case START_STATE:
            if (byte == FLAG)
                state = FIRST_FLAG_STATE;
            break;

        case FIRST_FLAG_STATE:
            if (byte == A_RX)
                state = ADDRESS_STATE;
            else if (byte != FLAG)
                state = START_STATE;
            break;

        case ADDRESS_STATE:
            if (byte == CONTROL_RR(0) || byte == CONTROL_RR(1) ||
                byte == CONTROL_REJ(0) || byte == CONTROL_REJ(1))
            {
                *control = byte;
                state = CONTROL_STATE;
            }
            else if (byte == FLAG)
            {
                state = FIRST_FLAG_STATE;
            }
            else
            {
                state = START_STATE;
            }
            break;

        case CONTROL_STATE:
            if (byte == (A_RX ^ *control))
                state = BCC_STATE;
            else if (byte == FLAG)
                state = FIRST_FLAG_STATE;
            else
                state = START_STATE;
            break;

        case BCC_STATE:
            if (byte == FLAG)
                state = FINAL_STATE;
            else
                state = START_STATE;
            break;

        default:
            state = START_STATE;
            break;
        }
    }

    return state == FINAL_STATE ? 1 : 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Incorrect program usage\n"
               "Usage: %s <SerialPort> [message]\n"
               "Example: %s /dev/ttyS1 \"Hello\"\n",
               argv[0], argv[0]);
        return 1;
    }

    const char *serialPortName = argv[1];
    const unsigned char *payload = (argc >= 3)
                                       ? (const unsigned char *)argv[2]
                                       : (const unsigned char *)"Mensagem enviada em modo non-canonical";
    int payloadSize = (int)strlen((const char *)payload);

    if (payloadSize <= 0 || payloadSize > MAX_PAYLOAD_SIZE)
    {
        fprintf(stderr, "Payload size must be between 1 and %d bytes\n", MAX_PAYLOAD_SIZE);
        return 1;
    }

    int fd = open(serialPortName, O_RDWR | O_NOCTTY);
    if (fd < 0)
    {
        perror(serialPortName);
        return 1;
    }

    struct termios oldtio;
    if (configureNonCanonical(fd, &oldtio) == -1)
    {
        perror("configureNonCanonical");
        close(fd);
        return 1;
    }

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        perror("sigaction");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return 1;
    }

    unsigned char frame[MAX_FRAME_SIZE];
    unsigned char sequenceNumber = 0;
    int frameLen = buildInformationFrame(payload, payloadSize, sequenceNumber, frame);

    int success = FALSE;
    for (int attempt = 1; attempt <= MAX_RETRANSMISSIONS && !success; attempt++)
    {
        unsigned char control = 0;

        printf("[write_noncanonical] Attempt #%d: sending %d-byte payload...\n",
               attempt, payloadSize);

        if (writeAll(fd, frame, frameLen) < 0)
        {
            perror("write");
            break;
        }
        tcdrain(fd);

        timeoutFlag = FALSE;
        alarm(TIMEOUT_SECONDS);

        int result = readSupervisionFrame(fd, &control);
        alarm(0);

        if (result < 0)
        {
            perror("read");
            break;
        }

        if (result == 1 && control == CONTROL_RR((sequenceNumber + 1) % 2))
        {
            printf("[write_noncanonical] RR received. Frame accepted.\n");
            success = TRUE;
        }
        else if (result == 1 && control == CONTROL_REJ(sequenceNumber))
        {
            printf("[write_noncanonical] REJ received. Retransmitting...\n");
        }
        else
        {
            printf("[write_noncanonical] Timeout or unexpected response. Retransmitting...\n");
        }
    }

    if (!success)
        fprintf(stderr, "[write_noncanonical] Failed to send frame after %d attempts\n",
                MAX_RETRANSMISSIONS);

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");

    close(fd);
    return success ? 0 : 1;
}
