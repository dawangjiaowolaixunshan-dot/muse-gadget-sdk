/*
 * vproxy: carries the gadget's outbound TCP connections over a VMess node.
 *
 * Works under the existing networking without touching it. The linker wraps
 * lwip_getaddrinfo and lwip_connect (see CMakeLists.txt):
 *
 *  - getaddrinfo() for a TCP host name returns a fake address in 198.18.0.0/16
 *    and remembers which name it stands for, so polluted DNS doesn't matter.
 *  - connect() to a fake address goes to a listener on 127.0.0.1 instead. A
 *    relay task accepts it, opens a VMess connection to the node with the real
 *    host name, and pumps bytes both ways, sealing and opening VMess chunks.
 *
 * The app's socket stays an ordinary lwIP socket, so select(), timeouts and
 * TLS on top behave exactly as before. Numeric addresses, UDP, IPv6 and local
 * names (.local, .lan) go out directly.
 *
 * Configuration lives in NVS, set over the USB console:
 *   >vmess=<host>,<port>,<command key as 32 hex digits>
 *   >vmess=off
 *   >vmess?
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads the node from NVS and, if one is set, starts the relay. Call once NVS
// is up; the network can come later.
void vproxy_start(void);

// Console hook for the ">vmess..." lines; false for anything else.
bool vproxy_console_command(const char *line);

#ifdef __cplusplus
}
#endif
