// KBUS <-> MQTT: publishes every KBUS channel, the module list and the bus status on
// the broker and writes the outputs from <prefix>/.../set messages.
//
//   <prefix>/status                    online | offline (retained, last will)
//   <prefix>/di/<n>  <prefix>/do/<n>   true | false
//   <prefix>/ai/<n>  <prefix>/ao/<n>   0..65535 (raw process value)
//   <prefix>/<class>/all               JSON array of the whole class (MQTT_ARRAYS=1)
//   <prefix>/do/<n>/set, ao/<n>/set    write one output
//   <prefix>/do/all/set, ao/all/set    write several outputs in one KBUS cycle (JSON array, null = keep)
//   <prefix>/modules                   JSON list of the modules and their channels
//   <prefix>/module/<pos>/in           raw input bytes of a complex module (e.g. 750-495), binary
//   <prefix>/module/<pos>/out/set      raw output bytes for it (binary, exactly its output size)
//   <prefix>/bus/healthy|cycletime|configversion|statistics
//   <prefix>/bus/cycletime/set, bus/rescan/set, bus/resetstatistics/set
//   <prefix>/error                     rejected writes (not retained)
#ifndef KBUS_GW_MQTT_IO_H
#define KBUS_GW_MQTT_IO_H

#include <signal.h>

#include "config.h"

// Runs until *stop is set: connects (and reconnects) to the broker, publishes, handles
// writes, runs bus rescans. Call after gateway_start(). Returns 0.
int mqtt_io_run(const gw_config_t *cfg, volatile sig_atomic_t *stop);

#endif
