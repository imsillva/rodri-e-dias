#define _POSIX_C_SOURCE 200809L
// Read from serial port in non-canonical mode
// Adapted from the non-canonical example and from the link layer llread logic.

#include <errno.h>
#include <fcntl.h>
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

    // Polling-style read to keep the state machine responsive.
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

static int destuffBytes(unsigned char *destuffed, const unsigned char *frame, int frameLen)
{
    int out = 0;

    for (int i = 0; i < frameLen; i++)
    {
        if (frame[i] == ESC)
        {
            if (i + 1 >= frameLen)
                return -1;
            destuffed[out++] = frame[i + 1] ^ 0x20;
            i++;
        }
        else
        {
            destuffed[out++] = frame[i];
        }
    }

    return out;
}

static int sendSupervisionFrame(int fd, unsigned char control)
{
    unsigned char frame[5];

    frame[0] = FLAG;
    frame[1] = A_RX;
    frame[2] = control;
    frame[3] = A_RX ^ control;
    frame[4] = FLAG;

    if (writeAll(fd, frame, 5) < 0)
        return -1;

    tcdrain(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Incorrect program usage\n"
               "Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n",
               argv[0], argv[0]);
        return 1;
    }

    const char *serialPortName = argv[1];

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

    unsigned char frame[MAX_FRAME_SIZE];
    unsigned char destuffed[MAX_PAYLOAD_SIZE + 1];
    unsigned char packet[MAX_PAYLOAD_SIZE + 1];
    unsigned char control = 0;
    unsigned char expectedSequence = 0;

    State state = START_STATE;
    int frameIdx = 0;

    printf("[read_noncanonical] Waiting for an information frame...\n");

    while (TRUE)
    {
        unsigned char byte = 0;
        int bytes = read(fd, &byte, 1);

        if (bytes < 0)
        {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        }
        if (bytes == 0)
            continue;

        switch (state)
        {
        case START_STATE:
            frameIdx = 0;
            if (byte == FLAG)
                state = FIRST_FLAG_STATE;
            break;

        case FIRST_FLAG_STATE:
            if (byte == A_TX)
                state = ADDRESS_STATE;
            else if (byte != FLAG)
                state = START_STATE;
            break;

        case ADDRESS_STATE:
            if (byte == CONTROL_N(0) || byte == CONTROL_N(1))
            {
                control = byte;
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
            if (byte == (A_TX ^ control))
            {
                state = BCC_STATE;
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

        case BCC_STATE:
            if (byte == FLAG)
            {
                int destuffedLen = destuffBytes(destuffed, frame, frameIdx);
                if (destuffedLen < 1)
                {
                    fprintf(stderr, "[read_noncanonical] Invalid stuffing. Sending REJ.\n");
                    sendSupervisionFrame(fd, CONTROL_REJ(expectedSequence));
                    state = START_STATE;
                    frameIdx = 0;
                    break;
                }

                unsigned char receivedBCC2 = destuffed[destuffedLen - 1];
                unsigned char calculatedBCC2 = 0;
                for (int i = 0; i < destuffedLen - 1; i++)
                    calculatedBCC2 ^= destuffed[i];

                if (receivedBCC2 == calculatedBCC2)
                {
                    if (control == CONTROL_N(expectedSequence))
                    {
                        int packetLen = destuffedLen - 1;
                        memcpy(packet, destuffed, packetLen);
                        packet[packetLen] = '\0';

                        expectedSequence = (expectedSequence + 1) % 2;
                        if (sendSupervisionFrame(fd, CONTROL_RR(expectedSequence)) == -1)
                            perror("write RR");

                        printf("[read_noncanonical] Valid frame received (%d bytes).\n", packetLen);
                        printf("[read_noncanonical] Payload as text: %s\n", packet);
                        printf("[read_noncanonical] Payload in hex:");
                        for (int i = 0; i < packetLen; i++)
                            printf(" %02X", packet[i]);
                        printf("\n");

                        if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
                            perror("tcsetattr");
                        close(fd);
                        return 0;
                    }
                    else
                    {
                        printf("[read_noncanonical] Duplicate frame. Sending RR again.\n");
                        if (sendSupervisionFrame(fd, CONTROL_RR(expectedSequence)) == -1)
                            perror("write RR");
                    }
                }
                else
                {
                    fprintf(stderr, "[read_noncanonical] BCC2 error. Sending REJ.\n");
                    if (sendSupervisionFrame(fd, CONTROL_REJ(expectedSequence)) == -1)
                        perror("write REJ");
                }

                state = START_STATE;
                frameIdx = 0;
            }
            else
            {
                if (frameIdx >= MAX_FRAME_SIZE - 1)
                {
                    fprintf(stderr, "[read_noncanonical] Frame too large. Restarting state machine.\n");
                    state = START_STATE;
                    frameIdx = 0;
                }
                else
                {
                    frame[frameIdx++] = byte;
                }
            }
            break;

        default:
            state = START_STATE;
            frameIdx = 0;
            break;
        }
    }

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
        perror("tcsetattr");

    close(fd);
    return 1;
}
