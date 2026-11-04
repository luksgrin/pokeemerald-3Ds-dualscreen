/* The board's own network: a 3DS associates to it and finds the bridge's
 * session on it. Later the same network carries the Switch's leg (the LDN
 * beacon is a vendor information element on it, and the Switch's GBA keys
 * are the password), so the 3DS and the Switch share one access point. */
#ifndef BRIDGE_WIFI_H
#define BRIDGE_WIFI_H

void wifi_start_ap(void);

#endif
