#pragma once

#include "pax_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

void pb_gfx_pax_init(void);
void pb_gfx_pax_flush(void);
pax_buf_t *pb_gfx_pax_get_buf(void);

#ifdef __cplusplus
}
#endif
