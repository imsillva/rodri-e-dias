#include "application_layer.h"

#include <stdio.h>
#include <string.h>

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s tx <SerialPort> <ImageFile>\n", prog);
    printf("  %s rx <SerialPort> [OutputDir]\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    ApplicationLayerConfig config;
    memset(&config, 0, sizeof(config));
    config.link_config.port = argv[2];
    config.link_config.timeout = LL_TIMEOUT_SECS;
    config.link_config.numTransmissions = LL_MAX_RETRIES;

    if (strcmp(argv[1], "tx") == 0) {
        if (argc < 4) {
            print_usage(argv[0]);
            return 1;
        }
        config.link_config.role = LL_ROLE_TX;
        config.input_file = argv[3];
    } else if (strcmp(argv[1], "rx") == 0) {
        config.link_config.role = LL_ROLE_RX;
        config.output_dir = (argc >= 4) ? argv[3] : ".";
    } else {
        print_usage(argv[0]);
        return 1;
    }

    return (application_layer_run(&config) == 0) ? 0 : 1;
}
