/*
 * vproxy: see vproxy.h.
 */
#include "vproxy.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "vmess.h"

static const char *TAG = "vproxy";

int __real_lwip_getaddrinfo(const char *node, const char *serv, const struct addrinfo *hints,
                            struct addrinfo **res);
int __real_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen);

#define LISTEN_PORT 17890
#define FAKE_N 128          // host names that can have a fake address at once
#define PENDING_N 16        // connects waiting for the relay to accept them
#define SESS_N 8            // relayed connections at once
#define HOST_MAX 96
#define APP_READ 4096
#define RX_CAP (16384 + 2 + 16 + 64)
#define U2A_CAP 16384
#define A2U_CAP (VMESS_FIRST_MAX + APP_READ + VMESS_CHUNK_OVERHEAD)
#define CONNECT_TIMEOUT_US (10 * 1000000LL)
#define DRAIN_TIMEOUT_US (5 * 1000000LL)
#define TIME_VALID 1735689600   // 2025-01-01: anything earlier isn't set

// ---- configuration ----

// Static state lives in PSRAM (EXT_RAM_BSS_ATTR): this board has no internal
// RAM to spare once Muse is connected.
static EXT_RAM_BSS_ATTR struct {
    bool on;
    char host[128];
    uint16_t port;
    uint8_t ck[16];
} s_cfg;

static void cfg_load(void) {
    nvs_handle_t h;
    if (nvs_open("vproxy", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t en = 0;
    size_t hl = sizeof(s_cfg.host), kl = sizeof(s_cfg.ck);
    if (nvs_get_u8(h, "en", &en) == ESP_OK && en &&
        nvs_get_str(h, "host", s_cfg.host, &hl) == ESP_OK &&
        nvs_get_u16(h, "port", &s_cfg.port) == ESP_OK &&
        nvs_get_blob(h, "ck", s_cfg.ck, &kl) == ESP_OK && kl == 16) {
        s_cfg.on = true;
    }
    nvs_close(h);
}

// ---- fake addresses and pending connects ----

static SemaphoreHandle_t s_lock;
static char (*s_fake)[HOST_MAX];   // index i <-> 198.18.(i >> 8).(i & 255), i >= 1
static int s_fake_next = 1;

static EXT_RAM_BSS_ATTR struct {
    uint16_t lport, dport;
    int16_t idx;
    int64_t t;
} s_pending[PENDING_N];

static volatile bool s_running;
// lwIP's TCP/IP thread comes up after vproxy_start(), and a socket call before
// it asserts. The first getaddrinfo()/connect() anyone makes proves it's up:
// they set NET, the relay then listens and sets LISTEN.
static EventGroupHandle_t s_ev;
#define EV_CLOCK BIT2
#define EV_NET BIT0
#define EV_LISTEN BIT1
static volatile uint32_t s_n_ok, s_n_fail;

static int fake_index(const char *host) {
    for (int i = 1; i < FAKE_N; i++) {
        if (s_fake[i][0] && !strcasecmp(s_fake[i], host)) return i;
    }
    int i = s_fake_next;
    s_fake_next = s_fake_next + 1 >= FAKE_N ? 1 : s_fake_next + 1;
    strlcpy(s_fake[i], host, HOST_MAX);
    return i;
}

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

static bool is_numeric(const char *s) {
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s) && *s != '.' && *s != ':') return false;
    }
    return true;
}

static bool bypass(const char *node, const struct addrinfo *hints) {
    if (!s_running || !node || !*node || strlen(node) >= HOST_MAX) return true;
    if (hints && (hints->ai_family == AF_INET6 || hints->ai_socktype == SOCK_DGRAM ||
                  hints->ai_protocol == IPPROTO_UDP)) return true;
    if (is_numeric(node) || !strchr(node, '.')) return true;
    if (ends_with(node, ".local") || ends_with(node, ".lan") || ends_with(node, ".home.arpa") ||
        ends_with(node, ".localhost")) return true;
    return !strcasecmp(node, s_cfg.host);
}

int __wrap_lwip_getaddrinfo(const char *node, const char *serv, const struct addrinfo *hints,
                            struct addrinfo **res) {
    if (s_ev) xEventGroupSetBits(s_ev, EV_NET);
    if (bypass(node, hints)) return __real_lwip_getaddrinfo(node, serv, hints, res);
    // The connect() that follows needs the relay listening.
    xEventGroupWaitBits(s_ev, EV_LISTEN, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = fake_index(node);
    xSemaphoreGive(s_lock);
    char ip[20];
    snprintf(ip, sizeof(ip), "198.18.%d.%d", i >> 8, i & 255);
    // A numeric name: lwIP builds the addrinfo itself (and freeaddrinfo frees
    // it as usual) without asking DNS.
    return __real_lwip_getaddrinfo(ip, serv, hints, res);
}

int __wrap_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen) {
    if (s_ev) xEventGroupSetBits(s_ev, EV_NET);
    if (!s_running || !name || name->sa_family != AF_INET || namelen < sizeof(struct sockaddr_in)) {
        return __real_lwip_connect(s, name, namelen);
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)name;
    uint32_t ip = ntohl(in->sin_addr.s_addr);
    if ((ip >> 16) != ((198u << 8) | 18u)) return __real_lwip_connect(s, name, namelen);
    int idx = (int)(ip & 0xffff);
    char host_dbg[HOST_MAX] = "";
    bool valid = false;
    if (idx >= 1 && idx < FAKE_N) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_fake[idx][0]) {
            strlcpy(host_dbg, s_fake[idx], sizeof(host_dbg));
            valid = true;
        }
        xSemaphoreGive(s_lock);
    }
    if (!valid) {
        errno = EHOSTUNREACH;
        return -1;
    }
    // Bind first so the relay can tell, from the peer port it accepts, which
    // host this connection is for.
    struct sockaddr_in lo = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t ll = sizeof(lo);
    if (lwip_bind(s, (struct sockaddr *)&lo, sizeof(lo)) != 0 ||
        lwip_getsockname(s, (struct sockaddr *)&lo, &ll) != 0) {
        ESP_LOGW(TAG, "bind for %s failed: %d", host_dbg, errno);
        errno = EHOSTUNREACH;
        return -1;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = 0;
    for (int k = 0; k < PENDING_N; k++) {   // free slot, else the oldest
        if (!s_pending[k].lport) { slot = k; break; }
        if (s_pending[k].t < s_pending[slot].t) slot = k;
    }
    s_pending[slot].lport = ntohs(lo.sin_port);
    s_pending[slot].dport = ntohs(in->sin_port);
    s_pending[slot].idx = (int16_t)idx;
    s_pending[slot].t = now;
    xSemaphoreGive(s_lock);
    lo.sin_port = htons(LISTEN_PORT);
    return __real_lwip_connect(s, (struct sockaddr *)&lo, sizeof(lo));
}

static bool pending_take(uint16_t lport, char *host, uint16_t *dport) {
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < PENDING_N; k++) {
        if (s_pending[k].lport == lport) {
            strlcpy(host, s_fake[s_pending[k].idx], HOST_MAX);
            *dport = s_pending[k].dport;
            s_pending[k].lport = 0;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

// ---- time: VMess auth IDs must be within two minutes of the node's clock ----

// Anchor VMess time to a reply received during this boot. The RTC may
// retain a plausible but stale date across resets.
static time_t s_ntp_epoch;
static int64_t s_ntp_last, s_ntp_try;

static bool time_ok(void) { return s_ev && (xEventGroupGetBits(s_ev) & EV_CLOCK); }

static int64_t vmess_time(void) {
    return s_ntp_epoch + (esp_timer_get_time() - s_ntp_last) / 1000000;
}

static bool ntp_once(const char *server) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *ai = NULL;
    if (__real_lwip_getaddrinfo(server, "123", &hints, &ai) != 0 || !ai) return false;
    int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bool ok = false;
    if (fd >= 0) {
        struct timeval tv = {.tv_sec = 2};
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t pkt[48] = {0x23};   // LI 0, version 4, client
        uint8_t cookie[8];
        esp_fill_random(cookie, sizeof(cookie));
        memcpy(pkt + 40, cookie, sizeof(cookie));
        if (__real_lwip_connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            lwip_send(fd, pkt, sizeof(pkt), 0) == sizeof(pkt) &&
            lwip_recv(fd, pkt, sizeof(pkt), 0) == sizeof(pkt) && (pkt[0] & 7) == 4 &&
            (pkt[0] >> 6) != 3 && pkt[1] > 0 && pkt[1] < 16 &&
            memcmp(pkt + 24, cookie, sizeof(cookie)) == 0) {
            uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                            ((uint32_t)pkt[42] << 8) | pkt[43];
            struct timeval now = {.tv_sec = (time_t)(secs - 2208988800u)};
            if (now.tv_sec > TIME_VALID) {
                s_ntp_epoch = now.tv_sec;
                s_ntp_last = esp_timer_get_time();
                settimeofday(&now, NULL);
                xEventGroupSetBits(s_ev, EV_CLOCK);
                ok = true;
            }
        }
        lwip_close(fd);
    }
    lwip_freeaddrinfo(ai);
    return ok;
}

static void ntp_sync(void) {
    static const char *servers[] = {"ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org",
                                    "time.apple.com"};
    int64_t now = esp_timer_get_time();
    if (time_ok() && now - s_ntp_last < 6LL * 3600 * 1000000) return;
    if (s_ntp_try && now - s_ntp_try < 5 * 1000000) return;
    s_ntp_try = now;
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
        if (ntp_once(servers[i])) {
            ESP_LOGI(TAG, "time synced from %s: %lld", servers[i], (long long)s_ntp_epoch);
            return;
        }
    }
}

/* 独立后台低优任务执行 NTP 校时，避免最长 8 秒的阻塞超时拖慢主转发循环 */
static void ntp_task(void *arg) {
    (void)arg;
    xEventGroupWaitBits(s_ev, EV_NET, pdFALSE, pdTRUE, portMAX_DELAY);
    for (;;) {
        ntp_sync();
        vTaskDelay(pdMS_TO_TICKS(time_ok() ? 60000 : 5000));
    }
}

static atomic_bool s_wifi_dropped = false;

static void on_wifi_down(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    atomic_store(&s_wifi_dropped, true);
}

// ---- the node's address ----

static struct sockaddr_in s_node;
static int64_t s_node_t;

static bool node_addr(struct sockaddr_in *out) {
    int64_t now = esp_timer_get_time();
    if (!s_node_t || now - s_node_t > 600LL * 1000000) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai = NULL;
        if (__real_lwip_getaddrinfo(s_cfg.host, NULL, &hints, &ai) != 0 || !ai) {
            ESP_LOGW(TAG, "can't resolve the node");
            return s_node_t != 0;   // keep using the last address
        }
        memcpy(&s_node, ai->ai_addr, sizeof(s_node));
        s_node.sin_port = htons(s_cfg.port);
        s_node_t = now;
        lwip_freeaddrinfo(ai);
    }
    *out = s_node;
    return true;
}

// ---- relay ----

typedef struct {
    int app, up;
    uint8_t st;               // 0 free, 1 connecting to the node, 2 open
    bool app_eof, up_eof;
    int64_t t;
    char host[HOST_MAX];
    uint16_t dport;
    vmess_sess_t vm;
    uint8_t *a2u, *rx, *u2a;
    size_t a2u_len, a2u_off, rx_len, u2a_len, u2a_off;
} sess_t;

static EXT_RAM_BSS_ATTR sess_t s_sess[SESS_N];
static uint8_t *s_readbuf;

static void set_nonblock(int fd) {
    int fl = lwip_fcntl(fd, F_GETFL, 0);
    lwip_fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void sess_close(sess_t *s, const char *why) {
    if (s->app >= 0) lwip_close(s->app);
    if (s->up >= 0) lwip_close(s->up);
    if (why) {
        ESP_LOGW(TAG, "%s:%u closed: %s", s->host, s->dport, why);
        s_n_fail++;
    }
    vmess_end(&s->vm);
    heap_caps_free(s->a2u);
    heap_caps_free(s->rx);
    heap_caps_free(s->u2a);
    memset(s, 0, sizeof(*s));
    s->app = s->up = -1;
}

static void sess_accept(int lfd) {
    struct sockaddr_in peer;
    socklen_t pl = sizeof(peer);
    int app = lwip_accept(lfd, (struct sockaddr *)&peer, &pl);
    if (app < 0) return;
    sess_t *s = NULL;
    for (int i = 0; i < SESS_N; i++) {
        if (!s_sess[i].st) { s = &s_sess[i]; break; }
    }
    char host[HOST_MAX];
    uint16_t dport;
    if (!pending_take(ntohs(peer.sin_port), host, &dport) || !s) {
        ESP_LOGW(TAG, "%s", s ? "accepted a connection nobody asked for" : "too many connections");
        lwip_close(app);
        return;
    }
    s->app = app;
    s->up = -1;
    strlcpy(s->host, host, sizeof(s->host));
    s->dport = dport;
    s->t = esp_timer_get_time();
    s->a2u = heap_caps_malloc(A2U_CAP, MALLOC_CAP_SPIRAM);
    s->rx = heap_caps_malloc(RX_CAP, MALLOC_CAP_SPIRAM);
    s->u2a = heap_caps_malloc(U2A_CAP, MALLOC_CAP_SPIRAM);
    s->st = 1;
    struct sockaddr_in node;
    if (!s->a2u || !s->rx || !s->u2a) { sess_close(s, "no memory"); return; }
    if (!node_addr(&node)) { sess_close(s, "node unresolved"); return; }
    set_nonblock(app);
    s->up = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->up < 0) { sess_close(s, "no socket"); return; }
    set_nonblock(s->up);
    int one = 1;
    lwip_setsockopt(s->up, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    lwip_setsockopt(app, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (__real_lwip_connect(s->up, (struct sockaddr *)&node, sizeof(node)) != 0 && errno != EINPROGRESS) {
        s_node_t = 0;   // resolve again next time
        sess_close(s, "can't reach the node");
    }
}

static void sess_opened(sess_t *s) {
    int err = 0;
    socklen_t el = sizeof(err);
    lwip_getsockopt(s->up, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) {
        s_node_t = 0;
        sess_close(s, "node refused");
        return;
    }
    if (!time_ok()) { sess_close(s, "clock not set (NTP blocked?)"); return; }
    uint8_t rnd[VMESS_RAND_LEN];
    esp_fill_random(rnd, sizeof(rnd));
    int r = vmess_begin(&s->vm, s_cfg.ck, s->host, s->dport, vmess_time(), rnd, s->a2u,
                        A2U_CAP, &s->a2u_len);
    if (r) {
        ESP_LOGW(TAG, "vmess_begin: %d", r);
        sess_close(s, "vmess setup failed");
        return;
    }
    s->a2u_off = 0;
    s->st = 2;
    ESP_LOGI(TAG, "%s:%u via node", s->host, s->dport);
}

// Sends what's pending; false if the socket failed.
static bool flush(int fd, uint8_t *buf, size_t *len, size_t *off) {
    while (*off < *len) {
        int n = lwip_send(fd, buf + *off, *len - *off, 0);
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        *off += (size_t)n;
    }
    *len = *off = 0;
    return true;
}

// Opens what the node sent, one unit at a time, while there is room for it.
static bool parse(sess_t *s) {
    while (!s->u2a_len && s->rx_len) {
        size_t used, ol;
        int r = vmess_open(&s->vm, s->rx, s->rx_len, &used, s->u2a, U2A_CAP, &ol);
        if (r < 0) {
            ESP_LOGW(TAG, "%s: bad data from the node (%d)", s->host, r);
            return false;
        }
        if (r == 1) break;
        memmove(s->rx, s->rx + used, s->rx_len - used);
        s->rx_len -= used;
        s->u2a_len = ol;
        s->u2a_off = 0;
        if (r == 2) {
            s->up_eof = true;
            s->rx_len = 0;
        }
        if (s->vm.rx_count == 1 && used && ol) s_n_ok++;
    }
    return true;
}

static void sess_io(sess_t *s, fd_set *rs, fd_set *ws) {
    if (s->st == 1) {
        if (FD_ISSET(s->up, ws)) sess_opened(s);
        else if (esp_timer_get_time() - s->t > CONNECT_TIMEOUT_US) {
            s_node_t = 0;
            sess_close(s, "node timed out");
        }
        return;
    }
    // App -> node.
    if (s->a2u_len && FD_ISSET(s->up, ws) && !flush(s->up, s->a2u, &s->a2u_len, &s->a2u_off)) {
        s_node_t = 0;
        sess_close(s, "node send failed");
        return;
    }
    if (!s->a2u_len && !s->app_eof && FD_ISSET(s->app, rs)) {
        int n = lwip_recv(s->app, s_readbuf, APP_READ, 0);
        if (n > 0 || n == 0) {
            if (n == 0) { s->app_eof = true; s->t = esp_timer_get_time(); }
            if (vmess_seal(&s->vm, s_readbuf, (size_t)n, s->a2u, A2U_CAP, &s->a2u_len)) {
                sess_close(s, "seal failed");
                return;
            }
            s->a2u_off = 0;
            flush(s->up, s->a2u, &s->a2u_len, &s->a2u_off);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            s->app_eof = true;
        }
    }
    // Node -> app.
    if (!s->up_eof && FD_ISSET(s->up, rs) && s->rx_len < RX_CAP) {
        int n = lwip_recv(s->up, s->rx + s->rx_len, RX_CAP - s->rx_len, 0);
        if (n > 0) s->rx_len += (size_t)n;
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) s->up_eof = true;
    }
    if (!parse(s)) { sess_close(s, "bad data"); return; }
    if (s->u2a_len && !flush(s->app, s->u2a, &s->u2a_len, &s->u2a_off)) {
        sess_close(s, NULL);   // the app went away
        return;
    }
    if (!s->u2a_len) parse(s);
    // Done when the node is finished and everything reached the app (a partial
    // chunk left over can't be opened any more), or the app hung up and the
    // node took too long to finish.
    if ((s->up_eof && !s->u2a_len) ||
        (s->app_eof && esp_timer_get_time() - s->t > DRAIN_TIMEOUT_US)) {
        sess_close(s, NULL);
    }
}

static void relay_task(void *arg) {
    (void)arg;
    xEventGroupWaitBits(s_ev, EV_NET, pdFALSE, pdTRUE, portMAX_DELAY);
    int lfd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(LISTEN_PORT),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (lfd < 0 || lwip_bind(lfd, (struct sockaddr *)&a, sizeof(a)) || lwip_listen(lfd, 8)) {
        ESP_LOGE(TAG, "can't listen on 127.0.0.1:%d", LISTEN_PORT);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    set_nonblock(lfd);
    for (int i = 0; i < SESS_N; i++) s_sess[i].app = s_sess[i].up = -1;
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_down, NULL);
    xEventGroupSetBits(s_ev, EV_LISTEN);
    ESP_LOGI(TAG, "relaying through the node on port %u", s_cfg.port);
    for (;;) {
        if (atomic_exchange(&s_wifi_dropped, false)) {
            s_node_t = 0;
            for (int i = 0; i < SESS_N; i++) {
                if (s_sess[i].st) sess_close(&s_sess[i], "wifi lost");
            }
        }
        fd_set rs, ws;
        FD_ZERO(&rs);
        FD_ZERO(&ws);
        FD_SET(lfd, &rs);
        int maxfd = lfd;
        for (int i = 0; i < SESS_N; i++) {
            sess_t *s = &s_sess[i];
            if (!s->st) continue;
            if (s->st == 1) {
                FD_SET(s->up, &ws);
            } else {
                if (s->a2u_len) FD_SET(s->up, &ws);
                else if (!s->app_eof) FD_SET(s->app, &rs);
                if (s->u2a_len) FD_SET(s->app, &ws);
                else if (!s->up_eof) FD_SET(s->up, &rs);
            }
            if (s->up > maxfd) maxfd = s->up;
            if (s->app > maxfd) maxfd = s->app;
        }
        struct timeval tv = {.tv_sec = 1};
        int n = lwip_select(maxfd + 1, &rs, &ws, NULL, &tv);
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (FD_ISSET(lfd, &rs)) sess_accept(lfd);
        for (int i = 0; i < SESS_N; i++) {
            if (s_sess[i].st) sess_io(&s_sess[i], &rs, &ws);
        }
    }
}

void vproxy_start(void) {
    s_lock = xSemaphoreCreateMutex();
    cfg_load();
    if (s_cfg.on) s_ev = xEventGroupCreate();
    if (!s_cfg.on) {
        ESP_LOGI(TAG, "off (no node set)");
        return;
    }
    s_fake = heap_caps_calloc(FAKE_N, HOST_MAX, MALLOC_CAP_SPIRAM);
    s_readbuf = heap_caps_malloc(APP_READ, MALLOC_CAP_SPIRAM);
    if (!s_ev || !s_fake || !s_readbuf || psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "can't start");
        return;
    }
    // The relay's stack is in PSRAM: it never writes flash, and internal RAM
    // is what this board is short of.
    s_running = true;
    if (xTaskCreatePinnedToCoreWithCaps(relay_task, "vproxy", 10240, NULL, 5, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "can't start the relay task");
        return;
    }
    xTaskCreatePinnedToCoreWithCaps(ntp_task, "vproxy_ntp", 4096, NULL, 2, NULL, tskNO_AFFINITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// ---- console ----

static int unhex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) ||
            sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return s[2 * n] ? -1 : 0;
}

static void reply_restart(const char *msg) {
    printf("%s\n", msg);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

bool vproxy_console_command(const char *line) {
    if (!strcmp(line, "vmess?")) {
        int open = 0;
        for (int i = 0; i < SESS_N; i++) open += s_sess[i].st != 0;
        printf("@vmess {\"on\":%s,\"running\":%s,\"port\":%u,\"clock\":%s,\"open\":%d,\"ok\":%lu,\"fail\":%lu}\n",
               s_cfg.on ? "true" : "false", s_running ? "true" : "false", s_cfg.port,
               time_ok() ? "true" : "false", open, (unsigned long)s_n_ok, (unsigned long)s_n_fail);
        fflush(stdout);
        return true;
    }
    if (strncmp(line, "vmess=", 6) != 0) return false;
    const char *v = line + 6;
    nvs_handle_t h;
    if (nvs_open("vproxy", NVS_READWRITE, &h) != ESP_OK) {
        printf("@vmess error storage\n");
        fflush(stdout);
        return true;
    }
    if (!strcmp(v, "off")) {
        nvs_set_u8(h, "en", 0);
        nvs_commit(h);
        nvs_close(h);
        reply_restart("@vmess ok off");
        return true;
    }
    char host[128];
    unsigned port;
    const char *c1 = strchr(v, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    uint8_t ck[16];
    if (!c1 || !c2 || c1 - v <= 0 || (size_t)(c1 - v) >= sizeof(host) ||
        sscanf(c1 + 1, "%u", &port) != 1 || !port || port > 65535 || unhex(c2 + 1, ck, 16)) {
        nvs_close(h);
        printf("@vmess error format: >vmess=<host>,<port>,<32 hex>\n");
        fflush(stdout);
        return true;
    }
    memcpy(host, v, (size_t)(c1 - v));
    host[c1 - v] = 0;
    bool ok = nvs_set_str(h, "host", host) == ESP_OK && nvs_set_u16(h, "port", (uint16_t)port) == ESP_OK &&
              nvs_set_blob(h, "ck", ck, 16) == ESP_OK && nvs_set_u8(h, "en", 1) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    memset(ck, 0, sizeof(ck));
    if (!ok) {
        printf("@vmess error storage\n");
        fflush(stdout);
        return true;
    }
    reply_restart("@vmess ok");
    return true;
}
