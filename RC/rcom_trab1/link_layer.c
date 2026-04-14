#define _POSIX_C_SOURCE 200809L
#include "link_layer.h"
#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* State machines                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    START_STATE,
    FLAG_RCV_STATE,
    A_RCV_STATE,
    C_RCV_STATE,
    BCC_OK_STATE
} supervision_state_t;

typedef enum {
    IFRAME_START,
    IFRAME_FLAG_RCV,
    IFRAME_A_RCV,
    IFRAME_C_RCV,
    IFRAME_BCC1_OK,
    IFRAME_DATA_RCV,
    IFRAME_ESC_RCV
} iframe_state_t;

/* ------------------------------------------------------------------ */
/* Alarme                                                               */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_alarm_enabled = 0;
static volatile sig_atomic_t g_alarm_count   = 0;

static void alarm_handler(int signo)
{
    (void)signo;
    g_alarm_enabled = 0;
    g_alarm_count++;
    printf("Alarm #%d received\n", (int)g_alarm_count);
    fflush(stdout);
}

static int install_alarm_handler(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarm_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Injeção de erros e delay artificial                                  */
/* ------------------------------------------------------------------ */

/* Inicializa o gerador de números aleatórios uma única vez. */
static void init_rng(void)
{
    static int done = 0;
    if (!done) {
        srand((unsigned int)time(NULL));
        done = 1;
    }
}

/* Devolve 1 com probabilidade `p` (0.0 a 1.0), 0 caso contrário. */
static int random_event(double p)
{
    if (p <= 0.0) return 0;
    if (p >= 1.0) return 1;
    return ((double)rand() / (double)RAND_MAX) < p;
}

/* Corrompe o BCC2 da frame já construída (último byte antes do FLAG final).
 * A frame tem a estrutura:  FLAG A C BCC1 <payload+BCC2 possivelmente stuffed> FLAG
 * O último byte antes do FLAG final é sempre o BCC2 (ou a sua versão stuffed).
 * Inverter qualquer bit desse byte garante que o receptor deteta o erro. */
static void corrupt_bcc2(unsigned char *frame, int frame_len)
{
    /* O frame termina em FLAG (0x7E). O byte antes é o BCC2. */
    if (frame_len < 2) return;
    frame[frame_len - 2] ^= 0xFF;   /* inverte todos os bits → erro garantido */
    printf("[FER] BCC2 corrompido intencionalmente (teste de erro)\n");
    fflush(stdout);
}

/* Aplica o delay de propagação configurado (em ms). */
static void apply_propagation_delay(int delay_ms)
{
    if (delay_ms <= 0) return;
    printf("[DELAY] Atraso de propagação: %d ms\n", delay_ms);
    fflush(stdout);
    usleep((useconds_t)(delay_ms * 1000));
}

/* ------------------------------------------------------------------ */
/* Supervisão                                                           */
/* ------------------------------------------------------------------ */

static int send_supervision(int fd, unsigned char a, unsigned char c)
{
    unsigned char frame[5] = {FLAG, a, c, (unsigned char)(a ^ c), FLAG};
    printf("TX S-frame: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
           frame[0], frame[1], frame[2], frame[3], frame[4]);
    fflush(stdout);
    return serial_write_all(fd, frame, sizeof(frame));
}

/* ------------------------------------------------------------------ */
/* I-frame                                                              */
/* ------------------------------------------------------------------ */

static int build_iframe(unsigned char *frame,
                        const unsigned char *data,
                        int data_len,
                        int seq)
{
    if (data_len < 0 || data_len > LL_MAX_PAYLOAD_SIZE) return -1;

    unsigned char c    = seq ? C_I1 : C_I0;
    unsigned char bcc2 = 0x00;
    int idx = 0;

    frame[idx++] = FLAG;
    frame[idx++] = A_TX_CMD;
    frame[idx++] = c;
    frame[idx++] = (unsigned char)(A_TX_CMD ^ c);

    for (int i = 0; i < data_len; i++) bcc2 ^= data[i];

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

/*
 * send_iframe: constrói e envia uma I-frame.
 * Aplica delay e injeção de erro se configurados em `cfg`.
 * `cfg` pode ser NULL (sem efeitos de teste).
 */
static int send_iframe(int fd,
                       const unsigned char *data, int data_len, int seq,
                       const LinkLayerConfig *cfg)
{
    unsigned char frame[LL_MAX_FRAME_SIZE];
    int frame_len = build_iframe(frame, data, data_len, seq);
    if (frame_len < 0) return -1;

    /* 1. Delay de propagação ANTES de enviar */
    if (cfg != NULL)
        apply_propagation_delay(cfg->propagation_delay_ms);

    /* 2. Injeção de erro no BCC2 com probabilidade fer */
    if (cfg != NULL && random_event(cfg->fer))
        corrupt_bcc2(frame, frame_len);

    printf("TX I-frame (seq=%d, payload_len=%d)\n", seq, data_len);
    fflush(stdout);

    return serial_write_all(fd, frame, (size_t)frame_len);
}

/* ------------------------------------------------------------------ */
/* Leitura de supervision frame (com alarme)                            */
/* ------------------------------------------------------------------ */

static int read_supervision_frame(int fd,
                                  unsigned char expected_a,
                                  const unsigned char *accepted_c,
                                  int accepted_count,
                                  unsigned char *received_c)
{
    supervision_state_t state = START_STATE;
    unsigned char byte = 0, a = 0, c = 0;

    while (g_alarm_enabled) {
        int res = serial_read_byte(fd, &byte);
        if (res < 0)  return -1;
        if (res == 0) continue;

        printf("RX ctrl byte = 0x%02X\n", byte); fflush(stdout);

        switch (state) {
            case START_STATE:
                if (byte == FLAG) state = FLAG_RCV_STATE;
                break;
            case FLAG_RCV_STATE:
                if      (byte == FLAG)       { /* consecutivos OK */ }
                else if (byte == expected_a) { a = byte; state = A_RCV_STATE; }
                else                         { state = START_STATE; }
                break;
            case A_RCV_STATE: {
                if (byte == FLAG) { state = FLAG_RCV_STATE; break; }
                int ok = 0;
                for (int i = 0; i < accepted_count; i++)
                    if (byte == accepted_c[i]) { ok = 1; break; }
                if (ok) { c = byte; state = C_RCV_STATE; }
                else      state = START_STATE;
                break;
            }
            case C_RCV_STATE:
                if      (byte == FLAG)                        state = FLAG_RCV_STATE;
                else if (byte == (unsigned char)(a ^ c))      state = BCC_OK_STATE;
                else                                          state = START_STATE;
                break;
            case BCC_OK_STATE:
                if (byte == FLAG) {
                    if (received_c) *received_c = c;
                    return 1;
                }
                state = START_STATE;
                break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SET → UA  (receptor)                                                 */
/* ------------------------------------------------------------------ */

static int wait_for_set_and_reply_ua(LinkLayer *ll)
{
    supervision_state_t state = START_STATE;
    unsigned char byte = 0, a = 0, c = 0;

    while (1) {
        int res = serial_read_byte(ll->fd, &byte);
        if (res < 0)  return -1;
        if (res == 0) continue;

        printf("RX byte = 0x%02X\n", byte); fflush(stdout);

        switch (state) {
            case START_STATE:
                if (byte == FLAG) state = FLAG_RCV_STATE;
                break;
            case FLAG_RCV_STATE:
                if      (byte == FLAG)      { }
                else if (byte == A_TX_CMD)  { a = byte; state = A_RCV_STATE; }
                else                        { state = START_STATE; }
                break;
            case A_RCV_STATE:
                if      (byte == FLAG)  { state = FLAG_RCV_STATE; }
                else if (byte == C_SET) { c = byte; state = C_RCV_STATE; }
                else                    { state = START_STATE; }
                break;
            case C_RCV_STATE:
                if      (byte == FLAG)                   state = FLAG_RCV_STATE;
                else if (byte == (unsigned char)(a ^ c)) state = BCC_OK_STATE;
                else {
                    printf("BCC1 error: got 0x%02X expected 0x%02X\n",
                           byte, (unsigned char)(a ^ c));
                    fflush(stdout);
                    state = START_STATE;
                }
                break;
            case BCC_OK_STATE:
                if (byte == FLAG) {
                    printf("Valid SET received\n"); fflush(stdout);
                    if (send_supervision(ll->fd, A_RX_REPLY, C_UA) < 0) return -1;
                    printf("UA sent. Connection established.\n"); fflush(stdout);
                    return 0;
                }
                state = START_STATE;
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* DISC → DISC  e  UA  (receptor)                                       */
/* ------------------------------------------------------------------ */

static int wait_for_disc_and_reply_disc(LinkLayer *ll)
{
    supervision_state_t state = START_STATE;
    unsigned char byte = 0, a = 0, c = 0;

    while (1) {
        int res = serial_read_byte(ll->fd, &byte);
        if (res < 0)  return -1;
        if (res == 0) continue;

        printf("RX byte = 0x%02X\n", byte); fflush(stdout);

        switch (state) {
            case START_STATE:
                if (byte == FLAG) state = FLAG_RCV_STATE;
                break;
            case FLAG_RCV_STATE:
                if      (byte == FLAG)     { }
                else if (byte == A_TX_CMD) { a = byte; state = A_RCV_STATE; }
                else                       { state = START_STATE; }
                break;
            case A_RCV_STATE:
                if      (byte == FLAG)   { state = FLAG_RCV_STATE; }
                else if (byte == C_DISC) { c = byte; state = C_RCV_STATE; }
                else                     { state = START_STATE; }
                break;
            case C_RCV_STATE:
                if      (byte == FLAG)                   state = FLAG_RCV_STATE;
                else if (byte == (unsigned char)(a ^ c)) state = BCC_OK_STATE;
                else                                     state = START_STATE;
                break;
            case BCC_OK_STATE:
                if (byte == FLAG) {
                    printf("DISC received\n"); fflush(stdout);
                    if (send_supervision(ll->fd, A_RX_REPLY, C_DISC) < 0) return -1;
                    printf("DISC sent. Waiting for UA...\n"); fflush(stdout);
                    return 0;
                }
                state = START_STATE;
                break;
        }
    }
}

static int wait_for_ua_after_disc(LinkLayer *ll)
{
    supervision_state_t state = START_STATE;
    unsigned char byte = 0, a = 0, c = 0;

    while (1) {
        int res = serial_read_byte(ll->fd, &byte);
        if (res < 0)  return -1;
        if (res == 0) continue;

        printf("RX byte = 0x%02X\n", byte); fflush(stdout);

        switch (state) {
            case START_STATE:
                if (byte == FLAG) state = FLAG_RCV_STATE;
                break;
            case FLAG_RCV_STATE:
                if      (byte == FLAG)     { }
                else if (byte == A_TX_CMD) { a = byte; state = A_RCV_STATE; }
                else                       { state = START_STATE; }
                break;
            case A_RCV_STATE:
                if      (byte == FLAG) { state = FLAG_RCV_STATE; }
                else if (byte == C_UA) { c = byte; state = C_RCV_STATE; }
                else                   { state = START_STATE; }
                break;
            case C_RCV_STATE:
                if      (byte == FLAG)                   state = FLAG_RCV_STATE;
                else if (byte == (unsigned char)(a ^ c)) state = BCC_OK_STATE;
                else                                     state = START_STATE;
                break;
            case BCC_OK_STATE:
                if (byte == FLAG) {
                    printf("UA received. Disconnection complete.\n"); fflush(stdout);
                    return 0;
                }
                state = START_STATE;
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* API pública                                                           */
/* ------------------------------------------------------------------ */

int llopen(LinkLayer *ll, const LinkLayerConfig *config)
{
    if (ll == NULL || config == NULL || config->port == NULL) return -1;

    memset(ll, 0, sizeof(*ll));
    ll->config = *config;
    if (ll->config.timeout <= 0)          ll->config.timeout = LL_TIMEOUT_SECS;
    if (ll->config.numTransmissions <= 0) ll->config.numTransmissions = LL_MAX_RETRIES;
    ll->txSequence         = 0;
    ll->rxExpectedSequence = 0;

    init_rng();   /* inicializar RNG para FER */

    ll->fd = serial_open_and_configure(ll->config.port, &ll->oldtio);
    if (ll->fd < 0) return -1;

    if (ll->config.role == LL_ROLE_TX) {
        if (install_alarm_handler() < 0) {
            serial_restore_and_close(ll->fd, &ll->oldtio);
            ll->fd = -1;
            return -1;
        }

        unsigned char accepted_ua[] = {C_UA};
        unsigned char received_c    = 0;
        int connected = 0;
        g_alarm_count = 0; g_alarm_enabled = 0;

        while (!connected && g_alarm_count < ll->config.numTransmissions) {
            if (!g_alarm_enabled) {
                if (send_supervision(ll->fd, A_TX_CMD, C_SET) < 0) {
                    serial_restore_and_close(ll->fd, &ll->oldtio);
                    ll->fd = -1;
                    return -1;
                }
                g_alarm_enabled = 1;
                alarm(ll->config.timeout);
            }
            int res = read_supervision_frame(ll->fd, A_RX_REPLY, accepted_ua, 1, &received_c);
            if (res < 0) { serial_restore_and_close(ll->fd, &ll->oldtio); ll->fd = -1; return -1; }
            if (res == 1) {
                alarm(0); g_alarm_enabled = 0;
                connected = 1;
                printf("UA received. Connection established.\n"); fflush(stdout);
            }
        }
        if (!connected) {
            serial_restore_and_close(ll->fd, &ll->oldtio);
            ll->fd = -1;
            return -1;
        }
    } else {
        printf("Receiver ready on %s\n", ll->config.port); fflush(stdout);
        if (wait_for_set_and_reply_ua(ll) < 0) {
            serial_restore_and_close(ll->fd, &ll->oldtio);
            ll->fd = -1;
            return -1;
        }
    }

    return ll->fd;
}

int llwrite(LinkLayer *ll, const unsigned char *buffer, int length)
{
    if (ll == NULL || buffer == NULL || length < 0 || length > LL_MAX_PAYLOAD_SIZE) return -1;
    if (ll->config.role != LL_ROLE_TX) return -1;

    unsigned char positive_ack = (ll->txSequence == 0) ? C_RR1 : C_RR0;
    unsigned char negative_ack = (ll->txSequence == 0) ? C_REJ0 : C_REJ1;
    unsigned char accepted[2]  = {positive_ack, negative_ack};
    unsigned char received_c   = 0;

    g_alarm_count = 0; g_alarm_enabled = 0;

    while (g_alarm_count < ll->config.numTransmissions) {
        if (!g_alarm_enabled) {
            /* Passa a config para send_iframe aplicar delay e FER */
            if (send_iframe(ll->fd, buffer, length,
                            ll->txSequence, &ll->config) < 0)
                return -1;
            g_alarm_enabled = 1;
            alarm(ll->config.timeout);
        }

        int res = read_supervision_frame(ll->fd, A_RX_REPLY, accepted, 2, &received_c);
        if (res < 0) return -1;
        if (res == 1) {
            alarm(0); g_alarm_enabled = 0;
            if (received_c == positive_ack) {
                printf("Positive ACK received for seq=%d\n", ll->txSequence);
                fflush(stdout);
                ll->txSequence = 1 - ll->txSequence;
                return length;
            }
            if (received_c == negative_ack) {
                printf("Negative ACK (REJ) received for seq=%d. Retransmitting...\n",
                       ll->txSequence);
                fflush(stdout);
                g_alarm_enabled = 0;   /* força reenvio imediato */
            }
        }
    }
    return -1;
}

int llread(LinkLayer *ll, unsigned char *buffer, int max_len)
{
    if (ll == NULL || buffer == NULL || max_len <= 0) return -1;
    if (ll->config.role != LL_ROLE_RX) return -1;

    iframe_state_t state = IFRAME_START;
    unsigned char byte   = 0;
    unsigned char a      = 0;
    unsigned char c      = 0;
    unsigned char data[LL_MAX_FRAME_SIZE];
    int data_index = 0;

    while (1) {
        int res = serial_read_byte(ll->fd, &byte);
        if (res < 0)  return -1;
        if (res == 0) continue;

        printf("RX byte = 0x%02X\n", byte); fflush(stdout);

        switch (state) {
            case IFRAME_START:
                if (byte == FLAG) state = IFRAME_FLAG_RCV;
                break;

            case IFRAME_FLAG_RCV:
                if      (byte == FLAG)     { }
                else if (byte == A_TX_CMD) { a = byte; state = IFRAME_A_RCV; }
                else {
                    printf("Ignored after FLAG: 0x%02X\n", byte); fflush(stdout);
                    state = IFRAME_START;
                }
                break;

            case IFRAME_A_RCV:
                if      (byte == FLAG)   { state = IFRAME_FLAG_RCV; }
                else if (byte == C_I0 || byte == C_I1) { c = byte; state = IFRAME_C_RCV; }
                else if (byte == C_DISC) { return -2; }
                else {
                    printf("Unexpected C byte 0x%02X\n", byte); fflush(stdout);
                    state = IFRAME_START;
                }
                break;

            case IFRAME_C_RCV:
                if      (byte == FLAG)                   state = IFRAME_FLAG_RCV;
                else if (byte == (unsigned char)(a ^ c)) state = IFRAME_BCC1_OK;
                else {
                    printf("BCC1 error: got 0x%02X expected 0x%02X\n",
                           byte, (unsigned char)(a ^ c));
                    fflush(stdout);
                    state = IFRAME_START;
                }
                break;

            case IFRAME_BCC1_OK:
                if (byte == FLAG) {
                    printf("Unexpected short frame C=0x%02X\n", c); fflush(stdout);
                    state = IFRAME_START; data_index = 0;
                } else {
                    data[0]    = byte;
                    data_index = 1;
                    state = (byte == ESC) ? IFRAME_ESC_RCV : IFRAME_DATA_RCV;
                }
                break;

            case IFRAME_DATA_RCV:
                if (byte == FLAG) {
                    if (data_index < 1) {
                        printf("Invalid I-frame: missing BCC2\n"); fflush(stdout);
                        state = IFRAME_START; data_index = 0;
                        break;
                    }

                    unsigned char bcc2_read = data[data_index - 1];
                    unsigned char bcc2_calc = 0x00;
                    for (int i = 0; i < data_index - 1; i++) bcc2_calc ^= data[i];

                    int frame_seq = (c == C_I1) ? 1 : 0;
                    int is_new    = (frame_seq == ll->rxExpectedSequence);

                    printf("I-frame seq=%d payload_len=%d BCC2 read=0x%02X calc=0x%02X\n",
                           frame_seq, data_index - 1, bcc2_read, bcc2_calc);
                    fflush(stdout);

                    if (bcc2_read == bcc2_calc) {
                        unsigned char rr_c;
                        if (is_new) {
                            rr_c = (ll->rxExpectedSequence == 0) ? C_RR1 : C_RR0;
                            ll->rxExpectedSequence = 1 - ll->rxExpectedSequence;
                            printf("Accepted I(Ns=%d). Sending %s.\n",
                                   frame_seq, (rr_c == C_RR1) ? "RR1" : "RR0");
                            fflush(stdout);
                            if (send_supervision(ll->fd, A_RX_REPLY, rr_c) < 0) return -1;
                            int payload_len = data_index - 1;
                            if (payload_len > max_len) {
                                fprintf(stderr, "Payload too large for llread buffer\n");
                                return -1;
                            }
                            memcpy(buffer, data, (size_t)payload_len);
                            return payload_len;
                        }
                        rr_c = (ll->rxExpectedSequence == 1) ? C_RR1 : C_RR0;
                        printf("Duplicate I(Ns=%d). Re-sending %s.\n",
                               frame_seq, (rr_c == C_RR1) ? "RR1" : "RR0");
                        fflush(stdout);
                        if (send_supervision(ll->fd, A_RX_REPLY, rr_c) < 0) return -1;
                    } else {
                        if (is_new) {
                            unsigned char rej_c = (ll->rxExpectedSequence == 0) ? C_REJ0 : C_REJ1;
                            printf("BCC2 error on expected I(Ns=%d). Sending %s.\n",
                                   frame_seq, (rej_c == C_REJ0) ? "REJ0" : "REJ1");
                            fflush(stdout);
                            if (send_supervision(ll->fd, A_RX_REPLY, rej_c) < 0) return -1;
                        } else {
                            unsigned char rr_c = (ll->rxExpectedSequence == 1) ? C_RR1 : C_RR0;
                            printf("BCC2 error on duplicate I(Ns=%d). Re-sending %s.\n",
                                   frame_seq, (rr_c == C_RR1) ? "RR1" : "RR0");
                            fflush(stdout);
                            if (send_supervision(ll->fd, A_RX_REPLY, rr_c) < 0) return -1;
                        }
                    }
                    state = IFRAME_START; data_index = 0;
                } else {
                    if (data_index >= (int)sizeof(data)) {
                        printf("Buffer overflow. Dropping frame.\n"); fflush(stdout);
                        state = IFRAME_START; data_index = 0;
                    } else {
                        data[data_index++] = byte;
                        if (byte == ESC) state = IFRAME_ESC_RCV;
                    }
                }
                break;

            case IFRAME_ESC_RCV:
                if (byte == FLAG) {
                    printf("Invalid escape before FLAG. Dropping.\n"); fflush(stdout);
                    state = IFRAME_START; data_index = 0;
                } else {
                    if (data_index <= 0 || data_index > (int)sizeof(data)) {
                        state = IFRAME_START; data_index = 0;
                    } else {
                        data[data_index - 1] = (unsigned char)(byte ^ ESC_XOR);
                        state = IFRAME_DATA_RCV;
                    }
                }
                break;
        }
    }
}

int llclose(LinkLayer *ll)
{
    if (ll == NULL || ll->fd < 0) return -1;

    int status = 0;

    if (ll->config.role == LL_ROLE_TX) {
        unsigned char accepted_disc[] = {C_DISC};
        unsigned char received_c      = 0;
        int disc_done = 0;
        g_alarm_count = 0; g_alarm_enabled = 0;

        while (!disc_done && g_alarm_count < ll->config.numTransmissions) {
            if (!g_alarm_enabled) {
                if (send_supervision(ll->fd, A_TX_CMD, C_DISC) < 0) {
                    status = -1; break;
                }
                g_alarm_enabled = 1;
                alarm(ll->config.timeout);
            }
            int res = read_supervision_frame(ll->fd, A_RX_REPLY,
                                             accepted_disc, 1, &received_c);
            if (res < 0) { status = -1; break; }
            if (res == 1) {
                alarm(0); g_alarm_enabled = 0;
                disc_done = 1;
                printf("DISC received from receiver.\n"); fflush(stdout);
            }
        }
        if (status == 0 && !disc_done) status = -1;
        if (status == 0 && send_supervision(ll->fd, A_TX_CMD, C_UA) < 0) status = -1;
        if (status == 0) { printf("UA sent. Disconnection complete.\n"); fflush(stdout); }
    } else {
        if (wait_for_disc_and_reply_disc(ll) < 0) status = -1;
        else if (wait_for_ua_after_disc(ll) < 0)  status = -1;
    }

    sleep(1);
    serial_restore_and_close(ll->fd, &ll->oldtio);
    ll->fd = -1;
    return status;
}
