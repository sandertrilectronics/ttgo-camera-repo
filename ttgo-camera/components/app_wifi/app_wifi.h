#ifndef __APP_WIFI_H__
#define __APP_WIFI_H__

#include <stdint.h>

extern void wifi_init_sta(char *ssid, char *pass);

extern uint8_t wifi_sta_is_connected(void);

extern void app_flash_init(void);

#endif