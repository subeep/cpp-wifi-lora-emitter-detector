// ESP32-S3 OpenDroneID (ASTM F3411) Wi-Fi test transmitter.
//
// Sends a simulated aircraft circling a fixed point as Wi-Fi Beacon and/or NAN
// action frames, so the receiver in src/drones/remote_id.cpp can be tested over
// the air. Settings come from menuconfig and can be changed at runtime over the
// serial console (type "help").
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "odid_encode.h"

static const char *TAG = "odid_tx";

#define MODE_BEACON 1
#define MODE_NAN    2

typedef struct {
    int channel;
    int rate;        // Mbps, 5 means 5.5
    int mode;        // MODE_BEACON | MODE_NAN
    int interval_ms;
    int power_qdbm;  // quarter dBm
    int version;     // OpenDroneID protocol version 0..2
    bool running;
} tx_config_t;

static tx_config_t cfg = {
    .channel = CONFIG_ODID_CHANNEL,
    .rate = CONFIG_ODID_RATE_MBPS,
    .mode = (CONFIG_ODID_SEND_BEACON ? MODE_BEACON : 0) | (CONFIG_ODID_SEND_NAN ? MODE_NAN : 0),
    .interval_ms = CONFIG_ODID_INTERVAL_MS,
    .power_qdbm = CONFIG_ODID_TX_POWER_QDBM,
    .version = CONFIG_ODID_PROTOCOL_VERSION,
    .running = true,
};
// Set by the console task, applied by the transmit task.
static volatile bool reconfigure_radio = false;
static volatile int dump_frames = 1;

static uint8_t mac[6];
static char serial_id[21];
static char ssid[33];

static bool rate_to_phy(int mbps, wifi_phy_rate_t *out)
{
    switch (mbps) {
    case 1: *out = WIFI_PHY_RATE_1M_L; return true;
    case 2: *out = WIFI_PHY_RATE_2M_L; return true;
    case 5: *out = WIFI_PHY_RATE_5M_L; return true;
    case 11: *out = WIFI_PHY_RATE_11M_L; return true;
    case 6: *out = WIFI_PHY_RATE_6M; return true;
    case 9: *out = WIFI_PHY_RATE_9M; return true;
    case 12: *out = WIFI_PHY_RATE_12M; return true;
    case 18: *out = WIFI_PHY_RATE_18M; return true;
    case 24: *out = WIFI_PHY_RATE_24M; return true;
    case 36: *out = WIFI_PHY_RATE_36M; return true;
    case 48: *out = WIFI_PHY_RATE_48M; return true;
    case 54: *out = WIFI_PHY_RATE_54M; return true;
    default: return false;
    }
}

static void radio_start(void)
{
    wifi_phy_rate_t phy;
    if (!rate_to_phy(cfg.rate, &phy)) phy = WIFI_PHY_RATE_1M_L;
    ESP_ERROR_CHECK(esp_wifi_config_80211_tx_rate(WIFI_IF_STA, phy));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(cfg.channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(cfg.power_qdbm));
}

static void radio_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // never connects; raw TX only
    radio_start();
}

// The ESP32 has no real-time clock source here; start from the build time and
// let the host correct it with "time <unix seconds>".
static void clock_init(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day, year, hh, mm, ss;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    struct tm t = {.tm_year = year - 1900, .tm_mon = (int)((strstr(months, mon) - months) / 3),
                   .tm_mday = day, .tm_hour = hh, .tm_min = mm, .tm_sec = ss};
    struct timeval tv = {.tv_sec = mktime(&t)};
    settimeofday(&tv, NULL);
}

static double now_unix(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

static void simulate(double t, odid_location_t *loc)
{
    const double lat0 = CONFIG_ODID_CENTER_LAT_E7 / 1e7, lon0 = CONFIG_ODID_CENTER_LON_E7 / 1e7;
    const double radius = CONFIG_ODID_CIRCLE_RADIUS_M, speed = CONFIG_ODID_SPEED_DM_S / 10.0;
    const double theta = speed / radius * t;
    const double north = radius * cos(theta), east = radius * sin(theta);
    double heading = atan2(cos(theta), -sin(theta)) * 180.0 / M_PI;
    if (heading < 0) heading += 360.0;

    loc->latitude = lat0 + north / 111320.0;
    loc->longitude = lon0 + east / (111320.0 * cos(lat0 * M_PI / 180.0));
    loc->height = CONFIG_ODID_HEIGHT_M;
    loc->alt_geo = CONFIG_ODID_GROUND_ALT_M + CONFIG_ODID_HEIGHT_M;
    loc->alt_baro = loc->alt_geo;
    loc->direction = (float)heading;
    loc->speed_h = (float)speed;
    loc->speed_v = 0;
    loc->status = 2; // airborne
}

static size_t build_pack(uint8_t *pack, const odid_location_t *loc, double unix_now)
{
    uint8_t msgs[5][ODID_MSG_SIZE];
    const uint8_t v = (uint8_t)cfg.version;
    odid_location_t l = *loc;
    l.seconds_past_hour = (float)fmod(unix_now, 3600.0);
    odid_system_t sys = {
        .operator_lat = CONFIG_ODID_CENTER_LAT_E7 / 1e7,
        .operator_lon = CONFIG_ODID_CENTER_LON_E7 / 1e7,
        .operator_alt_geo = CONFIG_ODID_GROUND_ALT_M,
        .operator_location_type = 0, // takeoff
        .classification_type = 1,    // EU
        .category_eu = 1,            // open
        .class_eu = 1,               // C0
        .area_count = 1,
        .area_radius_m = 0,
        .area_ceiling = -1000,
        .area_floor = -1000,
        .timestamp_2019 = (uint32_t)(unix_now - 1546300800.0),
    };
    odid_basic_id(msgs[0], v, 1 /* serial number */, 2 /* multirotor */, serial_id);
    odid_location(msgs[1], v, &l);
    odid_system(msgs[2], v, &sys);
    odid_self_id(msgs[3], v, 0, CONFIG_ODID_SELF_ID);
    odid_operator_id(msgs[4], v, 0, CONFIG_ODID_OPERATOR_ID);
    return odid_pack(pack, v, (const uint8_t(*)[ODID_MSG_SIZE])msgs, 5);
}

static void print_frame(const char *kind, const uint8_t *f, size_t n)
{
    uint32_t fcs = odid_fcs(f, n);
    printf("MPDU %s ", kind);
    for (size_t i = 0; i < n; ++i) printf("%02x", f[i]);
    printf("%02x%02x%02x%02x\n", (unsigned)(fcs & 255), (unsigned)((fcs >> 8) & 255),
           (unsigned)((fcs >> 16) & 255), (unsigned)(fcs >> 24));
}

static void tx_task(void *arg)
{
    uint8_t pack[3 + 5 * ODID_MSG_SIZE];
    uint8_t frame[512];
    uint16_t seq = 0;
    uint8_t beacon_counter = 0, nan_counter = 0;
    uint32_t sent_beacon = 0, sent_nan = 0, failed = 0;
    int64_t last_status = 0;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        if (reconfigure_radio) {
            reconfigure_radio = false;
            ESP_ERROR_CHECK(esp_wifi_stop());
            radio_start();
            ESP_LOGI(TAG, "radio: channel %d, %d Mbps, %.2f dBm max", cfg.channel, cfg.rate, cfg.power_qdbm / 4.0);
        }
        const double unix_now = now_unix();
        const double t = esp_timer_get_time() / 1e6;
        odid_location_t loc;
        simulate(t, &loc);
        size_t pack_len = build_pack(pack, &loc, unix_now);
        const bool dump = dump_frames > 0;

        if (cfg.running && (cfg.mode & MODE_BEACON)) {
            size_t n = odid_build_beacon(frame, sizeof frame, mac, seq++ & 0xfff, ssid, (uint8_t)cfg.channel,
                                         (uint64_t)esp_timer_get_time(), beacon_counter++, pack, pack_len);
            if (dump) print_frame("beacon", frame, n);
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)n, false) == ESP_OK) ++sent_beacon; else ++failed;
        }
        if (cfg.running && (cfg.mode & MODE_NAN)) {
            vTaskDelay(pdMS_TO_TICKS(2)); // keep the two frames from queueing back to back
            size_t n = odid_build_nan_action(frame, sizeof frame, mac, seq++ & 0xfff, nan_counter++, pack, pack_len);
            if (dump) print_frame("nan", frame, n);
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)n, false) == ESP_OK) ++sent_nan; else ++failed;
        }
        if (dump) --dump_frames;

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_status >= 1000000) {
            last_status = now_us;
            printf("STATUS %s ch=%d rate=%dM ver=%d beacon=%lu nan=%lu fail=%lu lat=%.7f lon=%.7f hdg=%.0f\n",
                   cfg.running ? "tx" : "stopped", cfg.channel, cfg.rate, cfg.version, (unsigned long)sent_beacon,
                   (unsigned long)sent_nan, (unsigned long)failed, loc.latitude, loc.longitude, loc.direction);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(cfg.interval_ms));
    }
}

static void help(void)
{
    printf("commands:\n"
           "  status               show settings\n"
           "  ch <1-11>            2.4 GHz channel\n"
           "  rate <Mbps>          1 2 5 11 (DSSS/CCK) or 6 9 12 18 24 36 48 54 (OFDM)\n"
           "  mode beacon|nan|both transports to send\n"
           "  int <ms>             interval between transmissions (20-5000)\n"
           "  pwr <8-84>           max TX power in 0.25 dBm units\n"
           "  ver <0-2>            OpenDroneID protocol version\n"
           "  time <unix>          set the clock used for message timestamps\n"
           "  hex [n]              print the next n transmissions as hex MPDUs with FCS\n"
           "  start | stop         enable/disable transmission\n");
}

static void show_status(void)
{
    printf("CONFIG mac=" MACSTR " id=%s ssid=%s ch=%d rate=%dM mode=%s%s int=%dms pwr=%.2fdBm ver=%d %s\n",
           MAC2STR(mac), serial_id, ssid, cfg.channel, cfg.rate, (cfg.mode & MODE_BEACON) ? "beacon " : "",
           (cfg.mode & MODE_NAN) ? "nan" : "", cfg.interval_ms, cfg.power_qdbm / 4.0, cfg.version,
           cfg.running ? "running" : "stopped");
}

static void handle(char *line)
{
    char cmd[16] = {0}, arg[24] = {0};
    if (sscanf(line, "%15s %23s", cmd, arg) < 1) return;
    long n = strtol(arg, NULL, 10);
    if (!strcmp(cmd, "help")) help();
    else if (!strcmp(cmd, "status")) show_status();
    else if (!strcmp(cmd, "ch") && n >= 1 && n <= 11) { cfg.channel = (int)n; reconfigure_radio = true; }
    else if (!strcmp(cmd, "rate")) {
        wifi_phy_rate_t phy;
        if (rate_to_phy((int)n, &phy)) { cfg.rate = (int)n; reconfigure_radio = true; }
        else { printf("ERR unsupported rate\n"); return; }
    }
    else if (!strcmp(cmd, "mode") && !strcmp(arg, "beacon")) cfg.mode = MODE_BEACON;
    else if (!strcmp(cmd, "mode") && !strcmp(arg, "nan")) cfg.mode = MODE_NAN;
    else if (!strcmp(cmd, "mode") && !strcmp(arg, "both")) cfg.mode = MODE_BEACON | MODE_NAN;
    else if (!strcmp(cmd, "int") && n >= 20 && n <= 5000) cfg.interval_ms = (int)n;
    else if (!strcmp(cmd, "pwr") && n >= 8 && n <= 84) { cfg.power_qdbm = (int)n; reconfigure_radio = true; }
    else if (!strcmp(cmd, "ver") && arg[0] >= '0' && arg[0] <= '2' && !arg[1]) cfg.version = (int)n;
    else if (!strcmp(cmd, "time") && n > 1546300800L) {
        struct timeval tv = {.tv_sec = n};
        settimeofday(&tv, NULL);
    }
    else if (!strcmp(cmd, "hex")) dump_frames = n > 0 ? (int)n : 1;
    else if (!strcmp(cmd, "start")) cfg.running = true;
    else if (!strcmp(cmd, "stop")) cfg.running = false;
    else { printf("ERR unknown or invalid command; type help\n"); return; }
    printf("OK\n");
    if (strcmp(cmd, "hex") && strcmp(cmd, "help")) show_status();
}

static void console_task(void *arg)
{
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    ESP_ERROR_CHECK(uart_driver_install(port, 256, 0, 0, NULL, 0));
    char line[64];
    size_t len = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(port, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (len) handle(line);
            len = 0;
        } else if (len + 1 < sizeof line && isprint(c)) {
            line[len++] = (char)c;
        }
    }
}

void app_main(void)
{
    clock_init();
    radio_init();
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    if (CONFIG_ODID_SERIAL_ID[0]) snprintf(serial_id, sizeof serial_id, "%s", CONFIG_ODID_SERIAL_ID);
    else snprintf(serial_id, sizeof serial_id, "ESP32S3RID%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(ssid, sizeof ssid, "RID-%s", serial_id);
    printf("\nOpenDroneID test transmitter ready; type \"help\" for commands\n");
    show_status();
    xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
    xTaskCreate(tx_task, "odid_tx", 6144, NULL, 6, NULL);
}
