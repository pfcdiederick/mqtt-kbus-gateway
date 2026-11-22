// Module descriptors, see modules.h.
#include "modules.h"

#include <stddef.h>

static module_desc_t descs[] = {
    // 750-450: 4 RTD inputs, one 16-bit word per channel. Pt100 (default setting):
    // 0.1 degC per digit, below 0 degC two's complement, -2000..+8500 = -200..+850 degC.
    { 450, "temperature", "°C", 0.1, 1 },
};

void module_desc_init(const gw_config_t *cfg)
{
    for (size_t i = 0; i < sizeof(descs) / sizeof(descs[0]); ++i) {
        if (descs[i].type == 450) {
            descs[i].resolution = cfg->m450_resolution;
            descs[i].unit = cfg->m450_unit;
        }
    }
}

const module_desc_t *module_desc(uint16_t type)
{
    for (size_t i = 0; i < sizeof(descs) / sizeof(descs[0]); ++i)
        if (descs[i].type == type)
            return &descs[i];
    return NULL;
}

double module_value(const module_desc_t *d, uint16_t raw)
{
    return (d->is_signed ? (double)(int16_t)raw : (double)raw) * d->resolution;
}
