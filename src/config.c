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
    cfg->cycle_ms       = env_int("KBUS_CYCLE_MS", 5, 1, 40);
    cfg->settings_store = env_str_empty("KBUS_SETTINGS_STORE", "/home/kbus-gateway/kbus-settings.conf");
    cfg->rt_priority    = env_int("KBUS_PRIORITY", 40, 0, 99);
    cfg->simulation     = env_int("KBUS_SIMULATION", 0, 0, 1);
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
        if (sscanf(line, "CYCLE_MS=%d", &v) == 1) {
            if (v >= 1 && v <= 40) {
                if (v != cfg->cycle_ms)
                    LOG_INFO("cycle time %d ms from %s (KBUS_CYCLE_MS: %d ms)", v, cfg->settings_store,
                             cfg->cycle_ms);
                cfg->cycle_ms = v;
            } else {
                LOG_WARN("%s: CYCLE_MS=%d out of range 1..40, ignored", cfg->settings_store, v);
                rc = -1;
            }
        }
    }
    fclose(f);
    return rc;
}

int config_store_cycle(const gw_config_t *cfg, int cycle_ms)
{
    if (!*cfg->settings_store)
        return 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->settings_store);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        LOG_WARN("cannot store the cycle time in %s: %s", cfg->settings_store, strerror(errno));
        return -1;
    }
    fprintf(f, "# written by kbus-gateway (<prefix>/bus/cycletime/set); delete to use KBUS_CYCLE_MS again\n"
               "CYCLE_MS=%d\n", cycle_ms);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0 || rename(tmp, cfg->settings_store) != 0) {
        LOG_WARN("cannot store the cycle time in %s: %s", cfg->settings_store, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
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
    if (cfg->mqtt_priority > 0 && cfg->rt_priority > 0 && cfg->mqtt_priority >= cfg->rt_priority)
        LOG_WARN("MQTT_PRIORITY %d is not below KBUS_PRIORITY %d: MQTT traffic may delay the KBUS cycle",
                 cfg->mqtt_priority, cfg->rt_priority);
}
