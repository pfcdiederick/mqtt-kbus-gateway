// Module descriptors: the functional data points of a module type, on top of its
// raw channels. One entry per module type, so topics and code are generated the
// same way for every type:
//
//   <prefix>/module/<pos>/<function>/<n>     value in <unit> (retained)
//
// Supported now:
//   750-450  4 RTD inputs: temperature/<1..4> in degC (signed 16-bit, 0.1 degC per
//            digit; MODULE_450_RESOLUTION / MODULE_450_UNIT for other sensor settings)
//
// The 750-671 stepper controller has a driver of its own (stepper.h). Not yet: the
// 750-657 (IO-Link master); it gets its raw bytes (<prefix>/module/<pos>/in, out/set).
#ifndef KBUS_GW_MODULES_H
#define KBUS_GW_MODULES_H

#include <stdint.h>

#include "config.h"

typedef struct {
    uint16_t    type;          // terminal code, e.g. 450
    const char *function;      // topic level, e.g. "temperature"
    const char *unit;          // e.g. "°C"
    double      resolution;    // value = (int16) raw * resolution
    int         is_signed;     // raw word is two's complement
} module_desc_t;

// Apply the configuration (MODULE_450_...). Call once after config_load.
void module_desc_init(const gw_config_t *cfg);
// Descriptor of a module type, NULL = none (raw channels only).
const module_desc_t *module_desc(uint16_t type);
// Scaled value of one raw channel word.
double module_value(const module_desc_t *d, uint16_t raw);

#endif
