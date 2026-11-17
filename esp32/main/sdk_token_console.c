/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sdk_token_console.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "identity.h"
#include "vproxy.h"
#include "noise_control.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

// The chip's own USB port, where it has one and the log goes to it.
#define USJ_CONSOLE (SOC_USB_SERIAL_JTAG_SUPPORTED && \
                     (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG))
#if USJ_CONSOLE
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "link.sdk_token";

#define COMMAND "sdk_token="
// A command line: '>', the command and a token, with room to spare.
#define COMMAND_LINE_MAX 96


// Temporary: >avprobe=<VM path> logs the first bytes of a file on the VM, to
// see what the avatar images are (format and size).
static void avprobe_cb(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    (void)ctx;
    char hex[3 * 48 + 1] = {0};
    for (size_t i = 0; i < len && i < 48; i++) sprintf(hex + 3 * i, "%02x ", data[i]);
    printf("@avprobe status=%d len=%u end=%d %s\n", status, (unsigned)len, end, hex);
    fflush(stdout);
}

void avprobe_path(const char *vm_path) {
    static EXT_RAM_BSS_ATTR char path[4][300];
    static int n;
    char *p = path[n++ & 3];
    snprintf(p, 300, "/fs/raw/%s", vm_path);
    const char *headers[] = {"Range", "bytes=0-63", NULL};
    int64_t id = noise_ctrl_req_open("GET", p, headers, true, avprobe_cb, NULL);
    printf("@avprobe open %lld %s\n", (long long)id, vm_path);
    fflush(stdout);
}

static bool avprobe_command(const char *line) {
    if (strncmp(line, "avprobe=", 8) != 0) return false;
    static EXT_RAM_BSS_ATTR char path[300];
    snprintf(path, sizeof(path), "/fs/raw/%s", line + 8);
    const char *headers[] = {"Range", "bytes=0-63", NULL};
    int64_t id = noise_ctrl_req_open("GET", path, headers, true, avprobe_cb, NULL);
    printf("@avprobe open %lld\n", (long long)id);
    fflush(stdout);
    return true;
}


// Temporary: >avdump=<VM path> prints a VM file as base64 lines
// ("@avd <offset> <data>"), to look at the avatar on the computer.
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
static struct {
    SemaphoreHandle_t done;
    uint8_t *buf;
    size_t len, cap;
    int status;
} s_avd;
static EXT_RAM_BSS_ATTR char s_avd_path[300];

static void avd_cb(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    (void)ctx;
    if (status != 0) s_avd.status = status;
    if (len && s_avd.len + len <= s_avd.cap) {
        memcpy(s_avd.buf + s_avd.len, data, len);
        s_avd.len += len;
    }
    if (end || status < 0) xSemaphoreGive(s_avd.done);
}

static void avd_task(void *arg) {
    (void)arg;
    size_t chunk = noise_ctrl_req_max_body();
    if (chunk > 8192) chunk = 8192;
    s_avd.cap = chunk;
    s_avd.buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    // PSRAM, not .bss: internal RAM is what this board is short of.
    char *b64 = heap_caps_malloc(11000 + 16, MALLOC_CAP_SPIRAM);
    size_t off = 0;
    for (;;) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)off, (unsigned)(off + chunk - 1));
        const char *headers[] = {"Range", range, NULL};
        s_avd.len = 0;
        s_avd.status = 0;
        if (!noise_ctrl_req_open("GET", s_avd_path, headers, true, avd_cb, NULL)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        xSemaphoreTake(s_avd.done, pdMS_TO_TICKS(20000));
        if (s_avd.status != 206 && s_avd.status != 200) break;
        size_t ol = 0;
        mbedtls_base64_encode((unsigned char *)b64, 11000 + 16, &ol, s_avd.buf, s_avd.len);
        b64[ol] = 0;
        printf("@avd %u %s\n", (unsigned)off, b64);
        fflush(stdout);
        off += s_avd.len;
        if (s_avd.len < chunk) break;
    }
    printf("@avd end %u status=%d\n", (unsigned)off, s_avd.status);
    fflush(stdout);
    heap_caps_free(s_avd.buf);
    heap_caps_free(b64);
    vTaskDeleteWithCaps(NULL);
}

static bool avdump_command(const char *line) {
    if (strncmp(line, "avdump=", 7) != 0) return false;
    snprintf(s_avd_path, sizeof(s_avd_path), "/fs/raw/%s", line + 7);
    if (!s_avd.done) s_avd.done = xSemaphoreCreateBinary();
    xTaskCreateWithCaps(avd_task, "avdump", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return true;
}

bool sdk_token_console_command(const char *line) {
    // The VMess node lines share this console (components/vproxy).
    if (vproxy_console_command(line)) return true;
    if (avprobe_command(line)) return true;
    if (avdump_command(line)) return true;
    if (strncmp(line, COMMAND, strlen(COMMAND)) != 0) return false;
    const char *token = line + strlen(COMMAND);
    if (!identity_sdk_token_valid(token)) {
        ESP_LOGW(TAG, "not an SDK token");
        printf("@sdk_token error invalid\n");
        fflush(stdout);
        return true;
    }
    if (!identity_set_sdk_token(token)) {
        ESP_LOGE(TAG, "couldn't store the SDK token");
        printf("@sdk_token error storage\n");
        fflush(stdout);
        return true;
    }
    ESP_LOGI(TAG, "stored SDK token %.12s, restarting to use it", token);
    printf("@sdk_token ok\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));   // let the answer reach the host
    esp_restart();
    return true;
}

// Collects bytes into a line and runs it on '\n' if it starts with '>'.
// Longer lines are dropped.
typedef struct {
    char buf[COMMAND_LINE_MAX];
    size_t len;
    bool overflow;
} line_reader_t;

static void feed(line_reader_t *r, char c) {
    if (c == '\r') return;
    if (c != '\n') {
        if (r->len < sizeof(r->buf) - 1) {
            r->buf[r->len++] = c;
        } else {
            r->overflow = true;
        }
        return;
    }
    r->buf[r->len] = '\0';
    if (!r->overflow && r->buf[0] == '>') sdk_token_console_command(r->buf + 1);
    r->len = 0;
    r->overflow = false;
}

#if CONFIG_ESP_CONSOLE_UART
static void uart_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

#if USJ_CONSOLE
static void usj_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

void sdk_token_console_start(void) {
#if CONFIG_ESP_CONSOLE_UART
    // With the driver installed, the log goes through it too, so it can't
    // interleave with what the driver sends.
    if (uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
        xTaskCreate(uart_task, "sdk_token_uart", 3072, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no console UART; can't set the SDK token over it");
    }
#endif
#if USJ_CONSOLE
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        xTaskCreate(usj_task, "sdk_token_usj", 3072, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no USB Serial/JTAG driver; can't set the SDK token over it");
    }
#endif
}
