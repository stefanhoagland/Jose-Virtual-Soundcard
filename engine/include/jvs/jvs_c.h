/* Plain C interface to the engine, used by the Mac app (Swift). */
#ifndef JVS_C_H
#define JVS_C_H

#ifdef __cplusplus
extern "C" {
#endif

#define JVS_MAX_CHANNELS 64

typedef struct jvs_session jvs_session;

typedef struct {
    char name[32];
    char ip[16];
} jvs_interface;

typedef struct {
    int running;
    int have_master;
    int locked;
    char grandmaster[32];
    char master_ip[16];
    double delay_us;
    double jitter_us;
    double rate_ppm;
    unsigned long long syncs;
} jvs_clock_status;

typedef struct {
    char key[64];
    char name[128];
    char addr[16];
    char origin[16];
    int port;
    int channels;
    int sample_rate;
    int bits;
    double ptime_ms;
} jvs_stream_info;

typedef struct {
    int active;
    char stream_name[128];
    int channels;
    unsigned long long packets;
    unsigned long long lost;
    double missing_pct;
    double arrival_avg_us;
    double arrival_max_us;
    double latency_ms;
    int playing;
    int recording;
    char wav_path[512];
    float peak_db[JVS_MAX_CHANNELS]; /* since last call; -120 = silence */
} jvs_rx_status;

typedef struct {
    int active;
    int flows;
    int channels;
    unsigned long long packets;
    unsigned long long late;
    unsigned long long resyncs;
} jvs_tx_status;

int jvs_list_interfaces(jvs_interface* out, int max);

jvs_session* jvs_create(void);
void jvs_destroy(jvs_session* s);

/* Starts the clock follower and stream discovery on a network port.
   Returns 0 on success; jvs_last_error() explains a failure. */
int jvs_start(jvs_session* s, const char* iface);
void jvs_stop(jvs_session* s);
const char* jvs_last_error(jvs_session* s);

void jvs_get_clock(jvs_session* s, jvs_clock_status* out);
int jvs_get_streams(jvs_session* s, jvs_stream_info* out, int max);

/* Receive one discovered stream. play_left/right are 1-based channels.
   wav_path may be NULL or empty for no recording. */
int jvs_rx_start(jvs_session* s, const char* stream_key, double latency_ms, int play, int play_left,
                 int play_right, const char* wav_path);
void jvs_rx_stop(jvs_session* s);
void jvs_get_rx(jvs_session* s, jvs_rx_status* out);

/* Send test tones: `flows` flows of `channels` channels each (Dante takes
   at most 8 per flow). base_addr like "239.69.200.1"; NULL for default. */
int jvs_tx_start(jvs_session* s, const char* name, int channels, int flows, const char* base_addr);
void jvs_tx_stop(jvs_session* s);
void jvs_get_tx(jvs_session* s, jvs_tx_status* out);

#ifdef __cplusplus
}
#endif

#endif
