// WAGO KBUS MQTT interface (kbus-gateway)
//
// Owns the KBUS of a WAGO PFC100/PFC200/PFC300 (ADI/DAL interface), runs the bus
// cycle in a real-time thread and makes every DI / DO / AI / AO channel, the
// module list and the bus status available on an MQTT broker (mosquitto on the
// controller). Outputs are written with <prefix>/do/<n>/set and <prefix>/ao/<n>/set.
// The serial interface COM1 is forwarded transparently (<prefix>/serial/com1/...).
// Runs natively on the PTXdist firmware (opkg package, tools/build-ipk.sh) or
// as a container (docker/gateway).
#include "backend.h"
#include "config.h"
#include "gateway.h"
#include "log.h"
#include "modules.h"
#include "mqtt_io.h"
#include "regcom.h"
#include "serial.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    log_init();
    LOG_INFO("WAGO KBUS MQTT interface %s starting", GW_VERSION);

    gw_config_t cfg;
    config_load(&cfg);
    config_load_stored(&cfg);           // cycle time written via <prefix>/bus/cycletime/set
    config_log(&cfg);
    module_desc_init(&cfg);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // Keep the real-time cycle free of page faults.
    if (cfg.rt_priority > 0 && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        LOG_WARN("mlockall failed; memory is not locked (needs IPC_LOCK / --privileged)");

    kbus_backend_t *backend = cfg.simulation ? backend_sim_create(&cfg) : backend_adi_create(&cfg);
    if (gateway_init(&cfg, backend) != 0) {
        LOG_ERROR("KBUS initialisation failed, exiting");
        return 1;
    }
    if (gateway_start() != 0) {
        gateway_stop();
        return 1;
    }

    serial_start(&cfg);                 // COM1 (SERIAL_DEVICE), transparent forwarding over MQTT
    regcom_start(&cfg);                 // register reads (read only): ai/<n>/status, module/<pos>/reg/get

    const int rc = mqtt_io_run(&cfg, &g_stop);

    LOG_INFO("shutting down");
    regcom_stop();
    serial_stop();
    gateway_stop();
    return rc == 0 ? 0 : 1;
}
