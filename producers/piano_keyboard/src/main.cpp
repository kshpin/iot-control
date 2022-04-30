#include <config.h>
#include <lwip/netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "driver/gpio.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#define LED_BUILTIN GPIO_NUM_1

bool connected = false;

static EventGroupHandle_t s_wifi_event_group;

esp_netif_t* sta_netif = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define MAX_RETRY 3
static int s_retry_num = 0;

unsigned long last_blink = 0;
bool on = false;

esp_mqtt_client_handle_t mqtt_client;

#define MQTT_EVENT_ID_LENGTH 128
char mqtt_event_id[MQTT_EVENT_ID_LENGTH];

#define MQTT_TOPIC_LENGTH 256
char mqtt_topic[MQTT_TOPIC_LENGTH];

#define MQTT_BODY_LENGTH 1024
char mqtt_body[MQTT_BODY_LENGTH];

unsigned long last_uptime_ping = 0;

// /configurations ------------------------------------------------

#define BLINK true

#define BLINK_PRIORITY 1
#define SENSOR_PRIORITY 2

// 0 - at most once (unreliable)
// 1 - at least once (requires indempotency) (DEFAULT)
// 2 - exactly once (slow)
#define MQTT_QOS 1

// configurations -------------------------------------------------

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
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

static void wifi_init() {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_sta_config_t sta = {WIFI_SSID, WIFI_PASSWORD};
    wifi_config_t wifi_config = {};
    wifi_config.sta = sta;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    connected = true;
}

static void mqtt_init() {
    esp_mqtt_client_config_t mqtt_cfg = {};
    memset((void*)&mqtt_cfg, 0, sizeof(esp_mqtt_client_config_t));
    mqtt_cfg.uri = MQTT_ADDRESS;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(mqtt_client);
}

void blink_loop() {
    int64_t cur_millis = esp_timer_get_time() / 1000;
    if (cur_millis - last_blink >= 500) {
        last_blink = cur_millis;
        if (on) {
            gpio_set_level(LED_BUILTIN, 0);
        } else {
            gpio_set_level(LED_BUILTIN, 1);
        }

        on = !on;
    }
}

void blink_loop_task(void* param) {
    while (true) {
        blink_loop();

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * @brief Perform the check of whatever event source the module uses, and if an event should be published, sets the
 * `mqtt_event_id` and `mqtt_body` strings appropriately
 *
 * @return whether the module should publish an event
 */
bool check_sensor() {
    int64_t cur_millis = esp_timer_get_time() / 1000;
    if (cur_millis - last_uptime_ping >= 1000) {
        last_uptime_ping = cur_millis;

        sprintf(mqtt_event_id, "uptime");
        sprintf(mqtt_body, "%llu", cur_millis);

        return true;
    }

    return false;
}

void sensor_loop() {
    if (check_sensor()) {
        sprintf(mqtt_topic, "%s/%s", DEVICE_ID, mqtt_event_id);
        esp_mqtt_client_publish(mqtt_client, mqtt_topic, mqtt_body, 0, MQTT_QOS, 0);
    }
}

void sensor_loop_task(void* param) {
    while (true) {
        sensor_loop();

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" void app_main();
void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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

    // network
    wifi_init();
    mqtt_init();

    // blink loop
    if (BLINK) {
        xTaskCreate(blink_loop_task, "blink", 10240, NULL, BLINK_PRIORITY, NULL);
    }

    // sensor loop
    xTaskCreate(sensor_loop_task, "sensor", 10240, NULL, SENSOR_PRIORITY, NULL);

    vTaskDelay(1);
}
