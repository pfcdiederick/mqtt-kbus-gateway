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
    // KBUS
    int         cycle_ms;        // KBUS_CYCLE_MS    (default 5, must stay well below 50 ms);
                                 //   a value written to <prefix>/bus/cycletime/set is stored in
                                 //   settings_store and wins over KBUS_CYCLE_MS
    const char *settings_store;  // KBUS_SETTINGS_STORE (default /home/kbus-gateway/kbus-settings.conf, "" = off)
    int         rt_priority;     // KBUS_PRIORITY    (default 40, SCHED_FIFO; 0 = no real-time)
    int         simulation;      // KBUS_SIMULATION  (default 0): simulated I/O, no hardware
    const char *dal_libs;        // KBUS_DAL_LIB     colon separated dlopen() candidates
    const char *info_libs;       // KBUS_INFO_LIB    colon separated dlopen() candidates
    const char *digital_offset;  // KBUS_DIGITAL_OFFSET_MODE auto|relative|absolute
} gw_config_t;

void config_load(gw_config_t *cfg);
void config_log(const gw_config_t *cfg);
// Stored settings (settings_store): CYCLE_MS=<n>. config_load_stored() applies them
// to cfg (call after config_load); config_store_cycle() writes them. 0 = ok.
int  config_load_stored(gw_config_t *cfg);
int  config_store_cycle(const gw_config_t *cfg, int cycle_ms);

#endif
