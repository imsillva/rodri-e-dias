#define _POSIX_C_SOURCE 200809L
#include "application_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FALSE 0
#define TRUE 1

typedef struct {
    FILE *fp;
    char output_dir[APP_MAX_PATH_LEN];
    char file_name[256];
    char output_path[APP_MAX_PATH_LEN];
    size_t expected_size;
    size_t bytes_written;
    int started;
    int ended;
} AppRxState;

static const char *safe_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;

    if (slash != NULL && slash[1] != '\0') {
        base = slash + 1;
    }
    if (backslash != NULL && backslash[1] != '\0' && backslash + 1 > base) {
        base = backslash + 1;
    }
    return base;
}

static size_t decode_size_value(const unsigned char *buf, int len)
{
    size_t value = 0;
    for (int i = 0; i < len; i++) {
        value = (value << 8) | buf[i];
    }
    return value;
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
    for (int i = 0; i < n; i++) {
        dst[2 + i] = tmp[n - 1 - i];
    }

    return 2 + n;
}

static int build_control_packet(unsigned char *packet,
                                unsigned char control,
                                const char *filename,
                                size_t file_size)
{
    const char *base = safe_basename(filename);
    size_t name_len = strlen(base);
    if (name_len > 255) {
        return -1;
    }

    int idx = 0;
    packet[idx++] = control;
    idx += encode_size_tlv(&packet[idx], file_size);
    packet[idx++] = APP_T_NAME;
    packet[idx++] = (unsigned char)name_len;
    memcpy(&packet[idx], base, name_len);
    idx += (int)name_len;

    if (idx > LL_MAX_PAYLOAD_SIZE) {
        return -1;
    }
    return idx;
}

static int build_data_packet(unsigned char *packet,
                             const unsigned char *data,
                             size_t data_len)
{
    if (data_len > APP_DATA_CHUNK) {
        return -1;
    }

    packet[0] = APP_DATA;
    packet[1] = (unsigned char)((data_len >> 8) & 0xFFu);
    packet[2] = (unsigned char)(data_len & 0xFFu);
    memcpy(&packet[3], data, data_len);
    return (int)(3 + data_len);
}

static int parse_control_packet(const unsigned char *payload,
                                int payload_len,
                                unsigned char expected_control,
                                char *file_name,
                                size_t file_name_size,
                                size_t *file_size)
{
    if (payload_len < 1 || payload[0] != expected_control) {
        return -1;
    }

    int idx = 1;
    int saw_name = FALSE;
    int saw_size = FALSE;

    while (idx + 2 <= payload_len) {
        unsigned char type = payload[idx++];
        unsigned char len = payload[idx++];
        if (idx + len > payload_len) {
            return -1;
        }

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
                              AppRxState *app)
{
    if (payload_len < 1) {
        return -1;
    }

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
        app->output_path[0] = '\0';
        strncat(app->output_path, app->output_dir,
                sizeof(app->output_path) - strlen(app->output_path) - 1);
        strncat(app->output_path, "/rx_",
                sizeof(app->output_path) - strlen(app->output_path) - 1);
        strncat(app->output_path, app->file_name,
                sizeof(app->output_path) - strlen(app->output_path) - 1);
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
            fprintf(stderr, "Warning: expected %zu bytes but wrote %zu bytes\n",
                    app->expected_size, app->bytes_written);
        }

        app->ended = TRUE;
        return 0;
    }

    fprintf(stderr, "Unknown application packet 0x%02X\n", control);
    return -1;
}

static int run_transmitter(const ApplicationLayerConfig *config)
{
    FILE *fp = fopen(config->input_file, "rb");
    if (fp == NULL) {
        perror(config->input_file);
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(fp);
        return -1;
    }
    long file_size_long = ftell(fp);
    if (file_size_long < 0) {
        perror("ftell");
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        perror("fseek");
        fclose(fp);
        return -1;
    }

    size_t file_size = (size_t)file_size_long;
    printf("Sending file: %s (%zu bytes)\n", config->input_file, file_size);
    fflush(stdout);

    LinkLayer ll;
    if (llopen(&ll, &config->link_config) < 0) {
        fprintf(stderr, "Failed to establish connection\n");
        fclose(fp);
        return -1;
    }

    unsigned char packet[LL_MAX_PAYLOAD_SIZE];

    int start_len = build_control_packet(packet, APP_START, config->input_file, file_size);
    if (start_len < 0 || llwrite(&ll, packet, start_len) < 0) {
        fprintf(stderr, "Failed to send START packet\n");
        fclose(fp);
        llclose(&ll);
        return -1;
    }

    unsigned char chunk[APP_DATA_CHUNK];
    size_t total_sent = 0;
    while (1) {
        size_t n = fread(chunk, 1, sizeof(chunk), fp);
        if (n > 0) {
            int data_len = build_data_packet(packet, chunk, n);
            if (data_len < 0 || llwrite(&ll, packet, data_len) < 0) {
                fprintf(stderr, "Failed to send DATA packet\n");
                fclose(fp);
                llclose(&ll);
                return -1;
            }
            total_sent += n;
            printf("Progress: %zu/%zu bytes sent\n", total_sent, file_size);
            fflush(stdout);
        }

        if (n < sizeof(chunk)) {
            if (ferror(fp)) {
                perror("fread");
                fclose(fp);
                llclose(&ll);
                return -1;
            }
            break;
        }
    }

    int end_len = build_control_packet(packet, APP_END, config->input_file, file_size);
    if (end_len < 0 || llwrite(&ll, packet, end_len) < 0) {
        fprintf(stderr, "Failed to send END packet\n");
        fclose(fp);
        llclose(&ll);
        return -1;
    }

    fclose(fp);
    if (llclose(&ll) < 0) {
        fprintf(stderr, "Failed to close connection cleanly\n");
        return -1;
    }
    return 0;
}

static int run_receiver(const ApplicationLayerConfig *config)
{
    LinkLayer ll;
    if (llopen(&ll, &config->link_config) < 0) {
        fprintf(stderr, "Failed to establish receiver connection\n");
        return -1;
    }

    AppRxState app;
    memset(&app, 0, sizeof(app));
    snprintf(app.output_dir, sizeof(app.output_dir), "%s",
             (config->output_dir != NULL) ? config->output_dir : ".");

    printf("Output directory: %s\n", app.output_dir);
    fflush(stdout);

    unsigned char payload[LL_MAX_PAYLOAD_SIZE];
    int finished = FALSE;

    while (!finished) {
        int len = llread(&ll, payload, sizeof(payload));
        if (len < 0) {
            fprintf(stderr, "llread failed\n");
            if (app.fp != NULL) {
                fclose(app.fp);
            }
            llclose(&ll);
            return -1;
        }

        if (process_app_packet(payload, len, &app) < 0) {
            if (app.fp != NULL) {
                fclose(app.fp);
            }
            llclose(&ll);
            return -1;
        }

        if (app.ended) {
            finished = TRUE;
        }
    }

    if (llclose(&ll) < 0) {
        fprintf(stderr, "Failed to close receiver connection cleanly\n");
        return -1;
    }
    return 0;
}

int application_layer_run(const ApplicationLayerConfig *config)
{
    if (config == NULL) {
        return -1;
    }

    if (config->link_config.role == LL_ROLE_TX) {
        if (config->input_file == NULL) {
            fprintf(stderr, "Missing input file for transmitter\n");
            return -1;
        }
        return run_transmitter(config);
    }

    return run_receiver(config);
}
