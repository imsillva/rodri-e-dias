#ifndef LINK_LAYER_H
#define LINK_LAYER_H

#include <stddef.h>
#include <termios.h>

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

#define LL_MAX_RETRIES      5
#define LL_TIMEOUT_SECS     3
#define LL_MAX_PAYLOAD_SIZE 256
#define LL_MAX_FRAME_SIZE   (4 + 2 * (LL_MAX_PAYLOAD_SIZE + 1) + 2)

typedef enum {
    LL_ROLE_TX = 0,
    LL_ROLE_RX = 1
} LinkLayerRole;

typedef struct {
    const char *port;
    LinkLayerRole role;
    int timeout;
    int numTransmissions;
} LinkLayerConfig;

typedef struct {
    int fd;
    struct termios oldtio;
    LinkLayerConfig config;
    int txSequence;
    int rxExpectedSequence;
} LinkLayer;

int llopen(LinkLayer *ll, const LinkLayerConfig *config);
int llwrite(LinkLayer *ll, const unsigned char *buffer, int length);
int llread(LinkLayer *ll, unsigned char *buffer, int max_len);
int llclose(LinkLayer *ll);

#endif
