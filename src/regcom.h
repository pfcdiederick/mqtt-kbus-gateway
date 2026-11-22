// Register communication, READ ONLY, in a worker thread of its own (each read blocks for
// about 20 ms, so it may neither run in the MQTT thread nor in the KBUS cycle thread):
//
//   - status of every analog input channel: register 6 of the channel's register table
//     (table = channel on the module - 1), at most once per MQTT_AI_STATUS_MS (>= 1000 ms)
//     per channel. A module whose first read fails (no register access) is left out
//     until the next bus scan.
//   - single register reads on request (<prefix>/module/<pos>/reg/get).
//
// Writing registers is deliberately not implemented.
#ifndef KBUS_GW_REGCOM_H
#define KBUS_GW_REGCOM_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define REGCOM_STATUS_REGISTER 6
#define REGCOM_QUEUE           16     // requests waiting or answered but not yet published
#define REGCOM_ID_MAX          72     // "id" of a request, as JSON text (quotes included)

enum { REGCOM_PENDING = 0, REGCOM_VALID, REGCOM_UNSUPPORTED };   // status of one AI channel

typedef struct {
    uint16_t pos, table, reg;
    char     id[REGCOM_ID_MAX];       // JSON literal ("x" or 12), "" = none
    int      ok;
    uint16_t value;
    const char *error;                // short reason when !ok
} regcom_result_t;

void regcom_start(const gw_config_t *cfg);   // after gateway_start()
void regcom_stop(void);                      // before gateway_stop()

// MQTT thread, after every bus (re)scan: new AI channel list, all states back to pending.
void regcom_layout(void);

// Status per AI channel (index = channel number - 1), for n channels.
void regcom_status(uint8_t *state, uint16_t *value, size_t n);

// Queue a read; 0 = queued, -1 = queue full, -2 = no register access in this gateway.
int  regcom_request(uint16_t pos, uint16_t table, uint16_t reg, const char *id);
// Next answered request: 1 = *r filled, 0 = none.
int  regcom_pop_result(regcom_result_t *r);
// Forget answers that were not published (broker connection lost).
void regcom_drop_results(void);

#endif
