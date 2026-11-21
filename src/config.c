#include "config.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *env_str(const char *name, const char *def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? v : def;
}

// Like env_str, but a variable that is set and empty stays empty ("" = off).
static const char *env_str_empty(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v != NULL ? v : def;
}

static int env_int(const char *name, int def, int min, int max)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return def;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || n < min || n > max) {
        LOG_WARN("%s='%s' is invalid (allowed %d..%d), using %d", name, v, min, max, def);
        return def;
    }
    return (int)n;
}

// The same checks as serial_valid_baud / serial_valid_format (serial.c), for the
// environment and the settings file.
static int valid_baud(int b)
{
    static const int ok[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400 };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); ++i)
        if (ok[i] == b)
            return 1;
    return 0;
}

static int valid_format(const char *f)
{
    return strlen(f) == 3 && f[0] >= '5' && f[0] <= '8' && strchr("NEO", f[1]) && (f[2] == '1' || f[2] == '2');
}

void config_load(gw_config_t *cfg)
{
    static char client_id[96];
    char host[64] = "pfc";
    gethostname(host, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    snprintf(client_id, sizeof(client_id), "kbus-gateway-%s", host);

    memset(cfg, 0, sizeof(*cfg));
    cfg->mqtt_host        = env_str("MQTT_HOST", "127.0.0.1");
    cfg->mqtt_port        = env_int("MQTT_PORT", 1883, 1, 65535);
    cfg->mqtt_client_id   = env_str("MQTT_CLIENT_ID", client_id);
    cfg->mqtt_username    = env_str("MQTT_USERNAME", "");
    cfg->mqtt_password    = env_str("MQTT_PASSWORD", "");
    cfg->mqtt_keepalive_s = env_int("MQTT_KEEPALIVE_S", 30, 5, 3600);
    cfg->topic_prefix     = env_str("MQTT_TOPIC_PREFIX", "kbus");
    cfg->retain           = env_int("MQTT_RETAIN", 1, 0, 1);
    cfg->publish_ms       = env_int("MQTT_PUBLISH_MS", 20, 5, 10000);
    cfg->analog_deadband  = env_int("MQTT_ANALOG_DEADBAND", 0, 0, 65535);
    cfg->arrays           = env_int("MQTT_ARRAYS", 1, 0, 1);
    cfg->stats_s          = env_int("MQTT_STATS_S", 10, 0, 86400);
    cfg->republish_s      = env_int("MQTT_REPUBLISH_S", 0, 0, 86400);
    cfg->write_enable     = env_int("MQTT_WRITE_ENABLE", 1, 0, 1);
    cfg->mqtt_log         = env_int("MQTT_LOG", 1, 0, 2);
    cfg->mqtt_priority    = env_int("MQTT_PRIORITY", 0, 0, 99);
    cfg->watchdog_s       = env_int("MQTT_WATCHDOG_S", 0, 0, 3600);
    cfg->raw_min_ms       = env_int("MQTT_RAW_MIN_MS", 100, 0, 60000);
    cfg->m450_unit        = env_str("MODULE_450_UNIT", "°C");
    cfg->m450_resolution  = 0.1;
    const char *res = getenv("MODULE_450_RESOLUTION");
    if (res != NULL && *res != '\0') {
        char *end = NULL;
        const double r = strtod(res, &end);
        if (end == res || *end != '\0' || r <= 0 || r > 1000)
            LOG_WARN("MODULE_450_RESOLUTION='%s' is invalid, using 0.1", res);
        else
            cfg->m450_resolution = r;
    }
    cfg->cycle_ms       = env_int("KBUS_CYCLE_MS", 5, 1, 40);
    cfg->settings_store = env_str_empty("KBUS_SETTINGS_STORE", "/home/kbus-gateway/kbus-settings.conf");
    cfg->rt_priority    = env_int("KBUS_PRIORITY", 40, 0, 99);
    cfg->simulation     = env_int("KBUS_SIMULATION", 0, 0, 1);
    cfg->serial_device  = env_str_empty("SERIAL_DEVICE", cfg->simulation ? "" : "/dev/serial");
    cfg->serial_baud    = env_int("SERIAL_BAUD", 19200, 1200, 230400);
    snprintf(cfg->serial_format, sizeof(cfg->serial_format), "%.3s", env_str("SERIAL_FORMAT", "8E1"));
    cfg->serial_frame_gap_ms = env_int("SERIAL_FRAME_GAP_MS", 10, 1, 1000);
    if (!valid_baud(cfg->serial_baud)) {
        LOG_WARN("SERIAL_BAUD=%d is not a standard baud rate, using 19200", cfg->serial_baud);
        cfg->serial_baud = 19200;
    }
    if (!valid_format(cfg->serial_format)) {
        LOG_WARN("SERIAL_FORMAT='%s' is invalid (e.g. 8E1, 8N1, 7E2), using 8E1", cfg->serial_format);
        snprintf(cfg->serial_format, sizeof(cfg->serial_format), "8E1");
    }
    cfg->dal_libs       = env_str("KBUS_DAL_LIB",
                                  "libdal.so:libdal.so.0:libdal.so.1");
    cfg->info_libs      = env_str("KBUS_INFO_LIB",
                                  "libdbuskbuscommon.so:libdbuskbuscommon.so.0:libdbuskbuscommon.so.1");
    cfg->digital_offset = env_str("KBUS_DIGITAL_OFFSET_MODE", "auto");

    // The prefix is used as the first topic level(s): no wildcards, no '/' at either end.
    static char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s", cfg->topic_prefix);
    size_t n = strlen(prefix);
    while (n > 0 && prefix[n - 1] == '/')
        prefix[--n] = '\0';
    const char *p = prefix;
    while (*p == '/')
        ++p;
    if (*p == '\0' || strpbrk(p, "+#") != NULL) {
        LOG_WARN("MQTT_TOPIC_PREFIX='%s' is invalid (empty or wildcard), using 'kbus'", cfg->topic_prefix);
        p = "kbus";
    }
    cfg->topic_prefix = p;
}

// What the settings file holds (0 = line not stored: the environment applies).
static int  stored_cycle_ms;
static int  stored_serial_baud;
static char stored_serial_format[4];
static int  stored_serial_gap;

int config_load_stored(gw_config_t *cfg)
{
    if (!*cfg->settings_store)
        return 0;
    FILE *f = fopen(cfg->settings_store, "r");
    if (f == NULL)
        return 0;                                         // nothing stored yet
    char line[128];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        int v;
        char fmt[8];
        if (sscanf(line, "CYCLE_MS=%d", &v) == 1) {
            if (v >= 1 && v <= 40) {
                if (v != cfg->cycle_ms)
                    LOG_INFO("cycle time %d ms from %s (KBUS_CYCLE_MS: %d ms)", v, cfg->settings_store,
                             cfg->cycle_ms);
                cfg->cycle_ms = stored_cycle_ms = v;
            } else {
                LOG_WARN("%s: CYCLE_MS=%d out of range 1..40, ignored", cfg->settings_store, v);
                rc = -1;
            }
        } else if (sscanf(line, "SERIAL_BAUD=%d", &v) == 1) {
            if (valid_baud(v)) {
                cfg->serial_baud = stored_serial_baud = v;
            } else {
                LOG_WARN("%s: SERIAL_BAUD=%d invalid, ignored", cfg->settings_store, v);
                rc = -1;
            }
        } else if (sscanf(line, "SERIAL_FRAME_GAP_MS=%d", &v) == 1) {
            if (v >= 1 && v <= 1000) {
                cfg->serial_frame_gap_ms = stored_serial_gap = v;
            } else {
                LOG_WARN("%s: SERIAL_FRAME_GAP_MS=%d out of range 1..1000, ignored", cfg->settings_store, v);
                rc = -1;
            }
        } else if (sscanf(line, "SERIAL_FORMAT=%7s", fmt) == 1) {
            if (valid_format(fmt)) {
                memcpy(stored_serial_format, fmt, 4);
                memcpy(cfg->serial_format, fmt, 4);
            } else {
                LOG_WARN("%s: SERIAL_FORMAT=%s invalid, ignored", cfg->settings_store, fmt);
                rc = -1;
            }
        }
    }
    fclose(f);
    return rc;
}

// Write the settings file: temporary file, fsync, rename (never a half file).
static int store_settings(const gw_config_t *cfg)
{
    if (!*cfg->settings_store)
        return 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->settings_store);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        LOG_WARN("cannot store the settings in %s: %s", cfg->settings_store, strerror(errno));
        return -1;
    }
    fprintf(f, "# written by kbus-gateway (<prefix>/bus/cycletime/set, <prefix>/serial/com1/.../set);\n"
               "# a missing line or a deleted file: the environment (KBUS_CYCLE_MS, SERIAL_...) applies\n");
    if (stored_cycle_ms)
        fprintf(f, "CYCLE_MS=%d\n", stored_cycle_ms);
    if (stored_serial_baud)
        fprintf(f, "SERIAL_BAUD=%d\n", stored_serial_baud);
    if (stored_serial_format[0])
        fprintf(f, "SERIAL_FORMAT=%s\n", stored_serial_format);
    if (stored_serial_gap)
        fprintf(f, "SERIAL_FRAME_GAP_MS=%d\n", stored_serial_gap);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0 || rename(tmp, cfg->settings_store) != 0) {
        LOG_WARN("cannot store the settings in %s: %s", cfg->settings_store, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int config_store_cycle(const gw_config_t *cfg, int cycle_ms)
{
    stored_cycle_ms = cycle_ms;
    return store_settings(cfg);
}

// Stores the values that are given (baud 0 / format NULL / gap 0 = keep what is stored).
int config_store_serial(const gw_config_t *cfg, int baud, const char *format, int frame_gap_ms)
{
    if (baud)
        stored_serial_baud = baud;
    if (format)
        snprintf(stored_serial_format, sizeof(stored_serial_format), "%.3s", format);
    if (frame_gap_ms)
        stored_serial_gap = frame_gap_ms;
    return store_settings(cfg);
}

void config_log(const gw_config_t *cfg)
{
    static const char *const log_names[] = { "none", "writes", "all" };
    LOG_INFO("config: broker=%s:%d client-id=%s user=%s prefix=%s retain=%d publish=%dms deadband=%d "
             "arrays=%d stats=%ds writes=%s log=%s mqtt-prio=%d cycle=%dms rt-prio=%d mode=%s",
             cfg->mqtt_host, cfg->mqtt_port, cfg->mqtt_client_id,
             *cfg->mqtt_username ? cfg->mqtt_username : "(anonymous)", cfg->topic_prefix, cfg->retain,
             cfg->publish_ms, cfg->analog_deadband, cfg->arrays, cfg->stats_s,
             cfg->write_enable ? "allowed" : "disabled", log_names[cfg->mqtt_log], cfg->mqtt_priority,
             cfg->cycle_ms, cfg->rt_priority, cfg->simulation ? "SIMULATION" : "kbus");
    if (cfg->watchdog_s > 0)
        LOG_INFO("config: write watchdog %d s: no write or watchdog/set for that long -> outputs 0", cfg->watchdog_s);
    if (*cfg->serial_device)
        LOG_INFO("config: COM1 %s %d %s, frame gap %d ms", cfg->serial_device, cfg->serial_baud,
                 cfg->serial_format, cfg->serial_frame_gap_ms);
    else
        LOG_INFO("config: COM1 off (SERIAL_DEVICE empty%s)", cfg->simulation ? ", simulation" : "");
    if (cfg->mqtt_priority > 0 && cfg->rt_priority > 0 && cfg->mqtt_priority >= cfg->rt_priority)
        LOG_WARN("MQTT_PRIORITY %d is not below KBUS_PRIORITY %d: MQTT traffic may delay the KBUS cycle",
                 cfg->mqtt_priority, cfg->rt_priority);
}
