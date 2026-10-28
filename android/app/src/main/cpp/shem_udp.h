/* =============================================================================
 * shem_udp.h - Multi-User Peer-to-Peer Live Coding Sync for Shem
 * Author: Jáchym Pešek
 * =============================================================================
 */

#ifndef SHEM_UDP_H
#define SHEM_UDP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool shem_udp_init(const char *username, int port);
void shem_udp_broadcast_code(const char *code);
bool shem_udp_pop_incoming(char *out_msg, int max_len);
void shem_udp_close(void);

#ifdef __cplusplus
}
#endif

#endif // SHEM_UDP_H
