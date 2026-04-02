#ifndef APPLICATION_LAYER_H
#define APPLICATION_LAYER_H

#include "link_layer.h"

#define APP_DATA   0x01
#define APP_START  0x02
#define APP_END    0x03
#define APP_T_SIZE 0x00
#define APP_T_NAME 0x01
#define APP_MAX_PATH_LEN 512
#define APP_DATA_CHUNK (LL_MAX_PAYLOAD_SIZE - 3)

typedef struct {
    LinkLayerConfig link_config;
    const char *input_file;
    const char *output_dir;
} ApplicationLayerConfig;

int application_layer_run(const ApplicationLayerConfig *config);

#endif
