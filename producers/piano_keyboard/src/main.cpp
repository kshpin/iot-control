#include <config.h>
#include <lwip/netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include <ESP32-USBSoftHost.hpp>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs_flash.h"

#define LED_BUILTIN GPIO_NUM_1

#define DP_P0 16  // always enabled
#define DM_P0 17  // always enabled
#define DP_P1 -1  // -1 to disable
#define DM_P1 -1  // -1 to disable
#define DP_P2 -1  // -1 to disable
#define DM_P2 -1  // -1 to disable
#define DP_P3 -1  // -1 to disable
#define DM_P3 -1  // -1 to disable

bool connected = false;

static EventGroupHandle_t s_wifi_event_group;

esp_netif_t* sta_netif = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define MAX_RETRY 3
static int s_retry_num = 0;

struct sockaddr_in dest_addr;
int sock;

// configurations --------------------

#define BLINK true
#define MONITOR false

unsigned long last_blink = 0;
bool on = false;

// /configurations -------------------

/* static void on_usb_detect(uint8_t usbNum, void *dev) {
    sDevDesc *device = (sDevDesc *)dev;

    udp.beginPacket(target_address, udp_port);

    udp.printf("New device detected on USB#%d\n", usbNum);
    udp.printf("desc.bcdUSB             = 0x%04x\n", device->bcdUSB);
    udp.printf("desc.bDeviceClass       = 0x%02x\n", device->bDeviceClass);
    udp.printf("desc.bDeviceSubClass    = 0x%02x\n", device->bDeviceSubClass);
    udp.printf("desc.bDeviceProtocol    = 0x%02x\n", device->bDeviceProtocol);
    udp.printf("desc.bMaxPacketSize0    = 0x%02x\n", device->bMaxPacketSize0);
    udp.printf("desc.idVendor           = 0x%04x\n", device->idVendor);
    udp.printf("desc.idProduct          = 0x%04x\n", device->idProduct);
    udp.printf("desc.bcdDevice          = 0x%04x\n", device->bcdDevice);
    udp.printf("desc.iManufacturer      = 0x%02x\n", device->iManufacturer);
    udp.printf("desc.iProduct           = 0x%02x\n", device->iProduct);
    udp.printf("desc.iSerialNumber      = 0x%02x\n", device->iSerialNumber);
    udp.printf("desc.bNumConfigurations = 0x%02x\n",
               device->bNumConfigurations);

    udp.endPacket();
    // if( device->iProduct == mySupportedIdProduct && device->iManufacturer ==
    // mySupportedManufacturer ) {
    //   myListenUSBPort = usbNum;
    // }
}

static void on_usb_data(uint8_t usbNum, uint8_t byte_depth, uint8_t *data,
                        uint8_t data_len) {
    // if( myListenUSBPort != usbNum ) return;

    udp.beginPacket(target_address, udp_port);

    udp.printf("in: ");
    for (int k = 0; k < data_len; k++) {
        udp.printf("0x%02x ", data[k]);
    }
    udp.printf("\n");

    udp.endPacket();
} */

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void initialise_wifi() {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_sta_config_t sta = {WIFI_SSID, WIFI_PASSWORD};
    wifi_config_t wifi_config = {};
    wifi_config.sta = sta;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    connected = true;
}

static void setup_udp() {
    dest_addr.sin_addr.s_addr = inet_addr(TARGET_ADDRESS);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    // to actually send:
    /* sendto(sock, payload, strlen(payload), 0, (struct sockaddr *)&dest_addr,
           sizeof(dest_addr)); */
}

void loop() {
    int64_t cur_millis = esp_timer_get_time() / 1000;
    if (cur_millis - last_blink >= 500) {
        last_blink = cur_millis;

        if (connected) {
            char payload[100];
            sprintf(payload, "ms since boot: %lli\n", cur_millis);

            printf("%s", payload);
            sendto(sock, payload, strlen(payload), 0,
                   (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        }

        if (BLINK) {
            if (on) {
                gpio_set_level(LED_BUILTIN, 0);
            } else {
                gpio_set_level(LED_BUILTIN, 1);
            }
        }

        on = !on;
    }
}

void loop_task(void* param) {
    while (true) {
        loop();
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" void app_main();
void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // led
    if (BLINK) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << LED_BUILTIN;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
    }

    // wifi
    initialise_wifi();
    setup_udp();

    xTaskCreate(loop_task, "loop", 10240, NULL, 1, NULL);

    vTaskDelay(1);
}
