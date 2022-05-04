#include <config.h>
#include <lwip/netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "driver/gpio.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
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

typedef enum {
    APP_GAP_STATE_IDLE = 0,
    APP_GAP_STATE_DEVICE_DISCOVERING,
    APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE,
    APP_GAP_STATE_SERVICE_DISCOVERING,
    APP_GAP_STATE_SERVICE_DISCOVER_COMPLETE,
} app_gap_state_t;

typedef struct {
    bool dev_found;
    uint8_t bdname_len;
    uint8_t eir_len;
    uint8_t rssi;
    uint32_t cod;
    uint8_t eir[ESP_BT_GAP_EIR_DATA_LEN];
    uint8_t bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    esp_bd_addr_t bda;
    app_gap_state_t state;
} app_gap_cb_t;

static app_gap_cb_t m_dev_info;

#define GAP_TAG "GAP"

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

void mqtt_send_debug(const char* fmt, ...) {
    va_list aptr;

    va_start(aptr, fmt);
    vsprintf(mqtt_body, fmt, aptr);
    va_end(aptr);

    sprintf(mqtt_topic, "%s/debug", DEVICE_ID);
    esp_mqtt_client_publish(mqtt_client, mqtt_topic, mqtt_body, 0, MQTT_QOS, 0);
}

static char* bda2str(esp_bd_addr_t bda, char* str, size_t size) {
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t* p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static char* uuid2str(esp_bt_uuid_t* uuid, char* str, size_t size) {
    if (uuid == NULL || str == NULL) {
        return NULL;
    }

    if (uuid->len == 2 && size >= 5) {
        sprintf(str, "%04x", uuid->uuid.uuid16);
    } else if (uuid->len == 4 && size >= 9) {
        sprintf(str, "%08x", uuid->uuid.uuid32);
    } else if (uuid->len == 16 && size >= 37) {
        uint8_t* p = uuid->uuid.uuid128;
        sprintf(str, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", p[15], p[14], p[13], p[12],
                p[11], p[10], p[9], p[8], p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    } else {
        return NULL;
    }

    return str;
}

static bool get_name_from_eir(uint8_t* eir, uint8_t* bdname, uint8_t* bdname_len) {
    uint8_t* rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void update_device_info(esp_bt_gap_cb_param_t* param) {
    char bda_str[18];
    uint32_t cod = 0;
    int32_t rssi = -129; /* invalid value */
    esp_bt_gap_dev_prop_t* p;

    mqtt_send_debug("Device found: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t*)(p->val);
                mqtt_send_debug("--Class of Device: 0x%x", cod);
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t*)(p->val);
                mqtt_send_debug("--RSSI: %d", rssi);
                break;
            case ESP_BT_GAP_DEV_PROP_BDNAME:
            default:
                break;
        }
    }

    /* search for device with MAJOR service class as "rendering" in COD */
    app_gap_cb_t* p_dev = &m_dev_info;
    if (p_dev->dev_found && 0 != memcmp(param->disc_res.bda, p_dev->bda, ESP_BD_ADDR_LEN)) {
        return;
    }

    if (!esp_bt_gap_is_valid_cod(cod) || (!(esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PHONE) &&
                                          !(esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV))) {
        return;
    }

    memcpy(p_dev->bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
    p_dev->dev_found = true;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                p_dev->cod = *(uint32_t*)(p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                p_dev->rssi = *(int8_t*)(p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_BDNAME: {
                uint8_t len = (p->len > ESP_BT_GAP_MAX_BDNAME_LEN) ? ESP_BT_GAP_MAX_BDNAME_LEN : (uint8_t)p->len;
                memcpy(p_dev->bdname, (uint8_t*)(p->val), len);
                p_dev->bdname[len] = '\0';
                p_dev->bdname_len = len;
                break;
            }
            case ESP_BT_GAP_DEV_PROP_EIR: {
                memcpy(p_dev->eir, (uint8_t*)(p->val), p->len);
                p_dev->eir_len = p->len;
                break;
            }
            default:
                break;
        }
    }

    if (p_dev->eir && p_dev->bdname_len == 0) {
        get_name_from_eir(p_dev->eir, p_dev->bdname, &p_dev->bdname_len);
        mqtt_send_debug("Found a target device, address %s, name %s", bda_str, p_dev->bdname);
        p_dev->state = APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE;
        mqtt_send_debug("Cancel device discovery ...");
        esp_bt_gap_cancel_discovery();
    }
}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    app_gap_cb_t* p_dev = &m_dev_info;
    char bda_str[18];
    char uuid_str[37];

    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            update_device_info(param);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                mqtt_send_debug("Device discovery stopped.");
                mqtt_send_debug("p_dev->dev_found: %d", p_dev->dev_found);
                if ((p_dev->state == APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE ||
                     p_dev->state == APP_GAP_STATE_DEVICE_DISCOVERING) &&
                    p_dev->dev_found) {
                    p_dev->state = APP_GAP_STATE_SERVICE_DISCOVERING;
                    mqtt_send_debug("Discover services ...");
                    esp_bt_gap_get_remote_services(p_dev->bda);
                }
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                mqtt_send_debug("Discovery started.");
            }
            break;
        }
        case ESP_BT_GAP_RMT_SRVCS_EVT: {
            if (memcmp(param->rmt_srvcs.bda, p_dev->bda, ESP_BD_ADDR_LEN) == 0 &&
                p_dev->state == APP_GAP_STATE_SERVICE_DISCOVERING) {
                p_dev->state = APP_GAP_STATE_SERVICE_DISCOVER_COMPLETE;
                if (param->rmt_srvcs.stat == ESP_BT_STATUS_SUCCESS) {
                    mqtt_send_debug("Services for device %s found", bda2str(p_dev->bda, bda_str, 18));
                    for (int i = 0; i < param->rmt_srvcs.num_uuids; i++) {
                        esp_bt_uuid_t* u = param->rmt_srvcs.uuid_list + i;
                        mqtt_send_debug("--%s", uuid2str(u, uuid_str, 37));
                        // ESP_LOGI(GAP_TAG, "--%d", u->len);
                    }
                } else {
                    mqtt_send_debug("Services for device %s not found", bda2str(p_dev->bda, bda_str, 18));
                }
            }
            break;
        }
        case ESP_BT_GAP_RMT_SRVC_REC_EVT:
        default: {
            mqtt_send_debug("event: %d", event);
            break;
        }
    }
}

static void bt_init() {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;

    esp_err_t ret;
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        mqtt_send_debug("esp_bt_controller_init failed: %s", esp_err_to_name(ret));
    }
    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        mqtt_send_debug("esp_bt_controller_enable failed: %s", esp_err_to_name(ret));
    }
    if ((ret = esp_bluedroid_init()) != ESP_OK) {
        mqtt_send_debug("esp_bluedroid_init failed: %s", esp_err_to_name(ret));
    }
    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        mqtt_send_debug("esp_bluedroid_enable failed: %s", esp_err_to_name(ret));
    }

    const char* dev_name = "ESP_GAP_INQRUIY";
    esp_bt_dev_set_device_name(dev_name);

    /* set discoverable and connectable mode, wait to be connected */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* register GAP callback function */
    esp_bt_gap_register_callback(bt_app_gap_cb);

    /* inititialize device information and status */
    app_gap_cb_t* p_dev = &m_dev_info;
    memset(p_dev, 0, sizeof(app_gap_cb_t));

    /* start to discover nearby Bluetooth devices */
    p_dev->state = APP_GAP_STATE_DEVICE_DISCOVERING;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
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

    // bluetooth
    bt_init();

    // blink loop
    if (BLINK) {
        xTaskCreate(blink_loop_task, "blink", 10240, NULL, BLINK_PRIORITY, NULL);
    }

    // sensor loop
    xTaskCreate(sensor_loop_task, "sensor", 10240, NULL, SENSOR_PRIORITY, NULL);

    vTaskDelay(1);
}
