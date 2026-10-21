/* C interface of the Rust bridge to Inferno (dante/src/lib.rs). */
#ifndef JVS_DANTE_H
#define JVS_DANTE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Jvsd Jvsd;

typedef struct {
    const char* name;         /* shown in Dante Controller, max 31 chars */
    const char* bind_ip;      /* IPv4 of the port wired to Dante */
    int rx_channels;
    int tx_channels;
    int sample_rate;
    int rx_latency_us;
    int tx_latency_us;
    const char* clock_socket; /* usrvclock socket path */
    int buffer_frames;
    int process_id;           /* >= 1 to run several devices on one IP */
    int alt_port;
} JvsdConfig;

Jvsd* jvsd_create(const JvsdConfig* cfg);
int jvsd_state(Jvsd* h); /* 1 running, 0 starting, -1 failed */
int jvsd_start_audio(Jvsd* h, uint64_t start_time);
const int32_t* jvsd_rx_buffer(Jvsd* h);
int32_t* jvsd_tx_buffer(Jvsd* h);
int jvsd_buffer_frames(Jvsd* h);
uint64_t jvsd_start_time(Jvsd* h);
void jvsd_name(Jvsd* h, char* out, int len);
int jvsd_subscribe(Jvsd* h, int local_channel, const char* tx_channel, const char* tx_device);
int jvsd_unsubscribe(Jvsd* h, int local_channel);
void jvsd_destroy(Jvsd* h);
/* Diagnostics */
int jvsd_set_log_file(const char* path); /* call before jvsd_create */
void jvsd_net_summary(char* out, int len); /* control traffic per port */

#ifdef __cplusplus
}
#endif
#endif
