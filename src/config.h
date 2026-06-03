#ifndef ULLLAS_CONFIG_H
#define ULLLAS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CHANNELS 16

typedef struct {
    bool is_sender;

    char backend[32];
    char device_name[256];
    unsigned int sample_rate;
    unsigned int bit_depth;
    unsigned int buffer_size;

    int in_channel_map[MAX_CHANNELS];
    int in_channel_count;
    int out_channel_map[MAX_CHANNELS];
    int out_channel_count;

    char target_addr[64];
    char bind_addr[64];
    uint16_t port;
    char iface_addr[64];
    bool use_multicast;
    unsigned int jitter_packets;
    bool jitter_set;       /* track explicit --jitter for sender warning */

    /* Opt-in stability features. All default off so behavior is
     * deterministic and the user is in control. */
    bool plc;             /* packet loss concealment with sample-hold + fade */
    unsigned int fec_group_size; /* 0 = off; otherwise [2..16] data packets per parity */
    bool drift_comp;      /* drop/insert single frames over time to track sender clock */

    bool list_devices;
    bool verbose;
} Config;

int config_parse(Config *cfg, int argc, char **argv);
void config_print_usage(const char *prog);

#ifdef __cplusplus
}
#endif

#endif
