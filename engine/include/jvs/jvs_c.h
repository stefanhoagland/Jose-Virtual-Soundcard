/* Plain C interface to the engine, used by the Mac app (Swift).
   One session = one Dante device on one network port. */
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

enum {
    JVS_PTP_AUTO = 0, /* PTPv1 (Dante's own clock) if present, otherwise PTPv2 */
    JVS_PTP_V1 = 1,
    JVS_PTP_V2 = 2
};

enum {
    JVS_STOPPED = 0,
    JVS_WAITING_FOR_CLOCK = 1,
    JVS_STARTING = 2,
    JVS_RUNNING = 3,
    JVS_FAILED = 4
};

typedef struct {
    const char* name;  /* shown in Dante Controller; letters, digits and '-' */
    const char* iface; /* network port, e.g. "en0" */
    int rx_channels;   /* 0..64 */
    int tx_channels;   /* 0..64 */
    int sample_rate;   /* 44100, 48000, 88200 or 96000 */
    int latency_us;    /* receive latency, like Dante Controller's setting */
    int ptp_mode;      /* JVS_PTP_* */
} jvs_config;

typedef struct {
    int state; /* JVS_STOPPED ... */
    char name[32];
    int rx_channels;
    int tx_channels;
    int sample_rate;
    int latency_us;

    /* Clock */
    int ptp_version; /* 1 or 2, 0 = none yet */
    int have_master;
    int locked;
    char grandmaster[32];
    char master_ip[16];
    double delay_us;
    double jitter_us;
    double rate_ppm;
    unsigned long long syncs;

    /* Test tools */
    int tones;
    int monitoring;
    int monitor_left, monitor_right;
    int recording;
    char wav_path[512];
    float peak_db[JVS_MAX_CHANNELS]; /* received levels since last call; -120 = silence */
} jvs_status;

int jvs_list_interfaces(jvs_interface* out, int max);

jvs_session* jvs_create(void);
void jvs_destroy(jvs_session* s);

/* Puts the device on the network. It then waits for the clock and appears
   in Dante Controller. Returns 0 on success; jvs_last_error() explains a failure. */
int jvs_start(jvs_session* s, const jvs_config* cfg);
void jvs_stop(jvs_session* s);
const char* jvs_last_error(jvs_session* s);

void jvs_get_status(jvs_session* s, jvs_status* out);

/* Test tones on every transmit channel (channel n = 200 + 100*n Hz). */
void jvs_set_tones(jvs_session* s, int on);

/* Play two received channels (1-based) on the Mac's speakers. */
int jvs_set_monitor(jvs_session* s, int on, int left, int right);

/* Record every received channel to a 32-bit float WAV file. */
int jvs_record_start(jvs_session* s, const char* wav_path);
void jvs_record_stop(jvs_session* s);

/* Route another Dante device's transmit channel to one of our receive
   channels (1-based), like a click in Dante Controller's routing grid. */
int jvs_subscribe(jvs_session* s, int local_channel, const char* tx_channel, const char* tx_device);

/* Makes a name Dante accepts: letters, digits and '-', at most 31 characters. */
void jvs_clean_name(const char* in, char* out, int out_size);

#ifdef __cplusplus
}
#endif

#endif
