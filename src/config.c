#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

static void config_set_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(Config));
    cfg->is_sender        = false;
    cfg->sample_rate      = 48000;
    cfg->bit_depth        = 24;
    cfg->buffer_size      = 128;
    cfg->port             = 9000;
    cfg->use_multicast    = true;
    cfg->jitter_packets   = 2;
    cfg->list_devices     = false;
    cfg->verbose          = false;
#ifdef __APPLE__
    strncpy(cfg->backend, "coreaudio", sizeof(cfg->backend) - 1);
#elif defined(__linux__)
    strncpy(cfg->backend, "jack", sizeof(cfg->backend) - 1);
#else
    strncpy(cfg->backend, "asio", sizeof(cfg->backend) - 1);
#endif
    strncpy(cfg->bind_addr, "0.0.0.0", sizeof(cfg->bind_addr) - 1);
    strncpy(cfg->iface_addr, "0.0.0.0", sizeof(cfg->iface_addr) - 1);
    cfg->device_name[0] = '\0';
    cfg->target_addr[0] = '\0';
}

static int parse_channels(const char *s, int *map, int max_ch) {
    int count = 0;
    char *copy = strdup(s);
    if (!copy) return 0;
    char *token = strtok(copy, ",");
    while (token && count < max_ch) {
        map[count++] = atoi(token);
        token = strtok(NULL, ",");
    }
    free(copy);
    return count;
}

void config_print_usage(const char *prog) {
    fprintf(stderr,
        "ULLLAS - Ultra Low Latency LAN Audio Streamer v1.0.0\n\n"
        "Usage: %s <mode> [options]\n\n"
        "Modes:\n"
        "  send     Capture local audio and stream to network\n"
        "  recv     Receive network audio and play locally\n\n"
        "Options:\n"
        "  --backend <name>       Audio backend: coreaudio (macOS), asio (Windows), jack (Linux)\n"
        "                          [default: auto (platform-dependent)]\n"
        "  --device <name>        Audio device name (list with --list-devices)\n"
        "  --in-channels <a,b,..>  Input channel indices to capture (sender)\n"
        "  --out-channels <a,b,..> Output channel indices to play to (receiver)\n"
        "  --sample-rate <hz>     Sample rate [default: 48000] (ignored on JACK)\n"
        "  --bit-depth <bits>     Bit depth: 16, 24, or 32 [default: 24]\n"
        "  --buffer <samples>     Audio buffer size in samples [default: 128]\n"
        "  --target <ip>           Target IP address (sender) or multicast group (receiver)\n"
        "                          [default: 239.77.77.77]\n"
        "  --bind <ip>            Bind address for receiver [default: 0.0.0.0]\n"
        "  --port <port>          UDP port for send and receive [default: 9000]\n"
        "  --iface <ip>           Outbound interface address for multicast [default: 0.0.0.0]\n"
        "  --unicast              Use unicast instead of multicast\n"
        "                          (sender: required when --target is not a multicast IP)\n"
        "  --jitter <packets>     Jitter buffer size in packets (receiver, 1-8) [default: 2]\n"
        "  --list-devices         List available audio devices and exit\n"
        "  --verbose              Verbose output (peak levels in status)\n\n"
        "Multicast (default): one sender reaches all receivers on the LAN.\n"
        "  Sender:   uillas send --in-channels 0,1\n"
        "  Receiver: uillas recv --out-channels 0,1\n"
        "  Both default to multicast group 239.77.77.77 port 9000 — just run and go.\n\n"
        "Unicast: point-to-point between two machines with known IP addresses.\n"
        "  Sender:   uillas send --in-channels 0,1 --target 192.168.1.100 --unicast\n"
        "  Receiver: uillas recv --out-channels 0,1\n"
        "  Use --unicast on the sender when the target is not in the multicast range.\n\n"
        "Examples:\n"
        "  %s send --backend asio --in-channels 0,1\n"
        "  %s recv --backend asio --out-channels 0,1\n"
        "  %s send --backend jack --in-channels 0,1 --target 192.168.1.100 --unicast\n",
        prog, prog, prog, prog);
}

int config_parse(Config *cfg, int argc, char **argv) {
    config_set_defaults(cfg);
    int in_set = 0, out_set = 0;

    if (argc < 2) {
        config_print_usage(argv[0]);
        return -1;
    }

    if (strcmp(argv[1], "send") == 0) {
        cfg->is_sender = true;
    } else if (strcmp(argv[1], "recv") == 0) {
        cfg->is_sender = false;
    } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        config_print_usage(argv[0]);
        return -1;
    } else {
        fprintf(stderr, "Unknown mode: %s (expected 'send' or 'recv')\n", argv[1]);
        config_print_usage(argv[0]);
        return -1;
    }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            strncpy(cfg->backend, argv[++i], sizeof(cfg->backend) - 1);
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            strncpy(cfg->device_name, argv[++i], sizeof(cfg->device_name) - 1);
        } else if (strcmp(argv[i], "--in-channels") == 0 && i + 1 < argc) {
            cfg->in_channel_count = parse_channels(argv[++i], cfg->in_channel_map, MAX_CHANNELS);
            in_set = 1;
        } else if (strcmp(argv[i], "--out-channels") == 0 && i + 1 < argc) {
            cfg->out_channel_count = parse_channels(argv[++i], cfg->out_channel_map, MAX_CHANNELS);
            out_set = 1;
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            cfg->sample_rate = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bit-depth") == 0 && i + 1 < argc) {
            cfg->bit_depth = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--buffer") == 0 && i + 1 < argc) {
            cfg->buffer_size = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            const char *val = argv[++i];
            const char *colon = strrchr(val, ':');
            if (colon) {
                fprintf(stderr, "Warning: port in --target is ignored, use --port instead\n");
                size_t ip_len = (size_t)(colon - val);
                if (ip_len >= sizeof(cfg->target_addr))
                    ip_len = sizeof(cfg->target_addr) - 1;
                memcpy(cfg->target_addr, val, ip_len);
                cfg->target_addr[ip_len] = '\0';
            } else {
                strncpy(cfg->target_addr, val, sizeof(cfg->target_addr) - 1);
            }
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            strncpy(cfg->bind_addr, argv[++i], sizeof(cfg->bind_addr) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg->port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            strncpy(cfg->iface_addr, argv[++i], sizeof(cfg->iface_addr) - 1);
        } else if (strcmp(argv[i], "--unicast") == 0) {
            cfg->use_multicast = false;
        } else if (strcmp(argv[i], "--jitter") == 0 && i + 1 < argc) {
            cfg->jitter_packets = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--list-devices") == 0) {
            cfg->list_devices = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            config_print_usage(argv[0]);
            return -1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            config_print_usage(argv[0]);
            return -1;
        }
    }

    if (cfg->bit_depth != 16 && cfg->bit_depth != 24 && cfg->bit_depth != 32) {
        fprintf(stderr, "Invalid bit depth: %u (must be 16, 24, or 32)\n", cfg->bit_depth);
        return -1;
    }

    if (cfg->sample_rate < 8000 || cfg->sample_rate > 384000) {
        fprintf(stderr, "Invalid sample rate: %u\n", cfg->sample_rate);
        return -1;
    }

    if (cfg->buffer_size < 8 || cfg->buffer_size > 4096) {
        fprintf(stderr, "Invalid buffer size: %u (must be 8-4096)\n", cfg->buffer_size);
        return -1;
    }

    if (cfg->jitter_packets < 1 || cfg->jitter_packets > 8) {
        fprintf(stderr, "Invalid jitter: %u (must be 1-8)\n", cfg->jitter_packets);
        return -1;
    }

    if (cfg->target_addr[0] == '\0') {
        strncpy(cfg->target_addr, "239.77.77.77", sizeof(cfg->target_addr) - 1);
    }

    if (cfg->port == 0) {
        cfg->port = 9000;
    }

    if (cfg->is_sender && out_set) {
        fprintf(stderr, "Warning: --out-channels has no effect in send mode (use --in-channels)\n");
    }
    if (!cfg->is_sender && in_set) {
        fprintf(stderr, "Warning: --in-channels has no effect in recv mode (use --out-channels)\n");
    }

    if (!cfg->list_devices) {
        if (cfg->is_sender && cfg->in_channel_count == 0) {
            cfg->in_channel_map[0] = 0;
            cfg->in_channel_map[1] = 1;
            cfg->in_channel_count = 2;
        }
        if (!cfg->is_sender && cfg->out_channel_count == 0) {
            cfg->out_channel_map[0] = 0;
            cfg->out_channel_map[1] = 1;
            cfg->out_channel_count = 2;
        }
    }

    return 0;
}
