#ifndef INTERNET_MGR_H
#define INTERNET_MGR_H

#include <stdbool.h>

void wifi_init_sta(void);
void wifi_start_task(void);
bool wifi_is_connected(void);
int wifi_get_rssi(void);

#endif /* INTERNET_MGR_H */
