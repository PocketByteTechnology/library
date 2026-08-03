#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void pb_sys_reboot_to_loader(void);
void pb_sys_handle_pending_reboot(void);

#ifdef __cplusplus
}
#endif
