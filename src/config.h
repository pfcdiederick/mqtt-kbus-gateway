// Runtime configuration, read from environment variables (/etc/kbus-gateway.conf).
#ifndef KBUS_GW_CONFIG_H
#define KBUS_GW_CONFIG_H

typedef struct {
    // MQTT broker (mosquitto on the controller)
    const char *mqtt_host;          // MQTT_HOST          (default 127.0.0.1)
    int         mqtt_port;          // MQTT_PORT          (default 1883)
    const char *mqtt_client_id;     // MQTT_CLIENT_ID     (default kbus-gateway-<hostname>)
    const char *mqtt_username;      // MQTT_USERNAME      (default "": anonymous)
    const char *mqtt_password;      // MQTT_PASSWORD
    int         mqtt_keepalive_s;   // MQTT_KEEPALIVE_S   (default 30)
    // Topics and publishing
    const char *topic_prefix;       // MQTT_TOPIC_PREFIX  (default "kbus")
    int         retain;             // MQTT_RETAIN        (default 1): values are retained messages
    int         publish_ms;         // MQTT_PUBLISH_MS    (default 20): changes are looked for every N ms
    int         analog_deadband;    // MQTT_ANALOG_DEADBAND (default 0): AI/AO published when changed by >= N
    int         arrays;             // MQTT_ARRAYS        (default 1): also <prefix>/<class>/all as JSON array
    int         stats_s;            // MQTT_STATS_S       (default 10): bus/statistics every N s, 0 = off
    int         republish_s;        // MQTT_REPUBLISH_S   (default 0): publish everything every N s, 0 = only changes
    int         write_enable;       // MQTT_WRITE_ENABLE  (default 1): 0 = no .../set topics (read only)
    int         mqtt_log;           // MQTT_LOG           (default 1): 0 none, 1 writes that change something, 2 all messages
    int         mqtt_priority;      // MQTT_PRIORITY      (default 0): SCHED_FIFO priority of the MQTT thread, 0 = normal
    int         raw_min_ms;         // MQTT_RAW_MIN_MS    (default 100): <prefix>/module/<pos>/in at most once per
                                    //   N ms per module (the latest bytes always follow); 0 = every change
    int         watchdog_s;         // MQTT_WATCHDOG_S    (default 0 = off): no write and no <prefix>/watchdog/set
                                    //   for N s (after the first one) -> all outputs 0 (safe state)
    // Module descriptors (modules.c)
    double      m450_resolution;    // MODULE_450_RESOLUTION (default 0.1): 750-450 value per digit
    const char *m450_unit;          // MODULE_450_UNIT    (default °C)
    // KBUS
    int         cycle_ms;        // KBUS_CYCLE_MS    (default 5, must stay well below 50 ms);
                                 //   a value written to <prefix>/bus/cycletime/set is stored in
                                 //   settings_store and wins over KBUS_CYCLE_MS
    const char *settings_store;  // KBUS_SETTINGS_STORE (default /home/kbus-gateway/kbus-settings.conf, "" = off)
    // Serial interface COM1 (serial.c): transparent forwarding
    const char *serial_device;   // SERIAL_DEVICE    (default /dev/serial; "" = off; off in simulation unless set)
    int         serial_baud;     // SERIAL_BAUD      (default 19200)
    char        serial_format[4];// SERIAL_FORMAT    (default 8E1: data bits, parity N/E/O, stop bits)
    int         serial_frame_gap_ms; // SERIAL_FRAME_GAP_MS (default 10): a pause this long ends a frame
                                 //   values written to <prefix>/serial/com1/.../set are stored in
                                 //   settings_store and win over these
    int         rt_priority;     // KBUS_PRIORITY    (default 40, SCHED_FIFO; 0 = no real-time)
    int         simulation;      // KBUS_SIMULATION  (default 0): simulated I/O, no hardware
    const char *dal_libs;        // KBUS_DAL_LIB     colon separated dlopen() candidates
    const char *info_libs;       // KBUS_INFO_LIB    colon separated dlopen() candidates
    const char *digital_offset;  // KBUS_DIGITAL_OFFSET_MODE auto|relative|absolute
} gw_config_t;

void config_load(gw_config_t *cfg);
void config_log(const gw_config_t *cfg);
// Stored settings (settings_store): CYCLE_MS=<n>, SERIAL_BAUD/SERIAL_FORMAT/SERIAL_FRAME_GAP_MS
// (each only once written over MQTT). config_load_stored() applies them to cfg (call after
// config_load); config_store_cycle() / config_store_serial() write the file (all lines). 0 = ok.
int  config_load_stored(gw_config_t *cfg);
int  config_store_cycle(const gw_config_t *cfg, int cycle_ms);
int  config_store_serial(const gw_config_t *cfg, int baud, const char *format, int frame_gap_ms);

#endif
