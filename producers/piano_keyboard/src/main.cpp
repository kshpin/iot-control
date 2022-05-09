#include <config.h>
#include <lwip/netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "NimBLEDevice.h"
#include "driver/gpio.h"
#include "esp_bt.h"
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
#define MQTT_BODY_LENGTH 1024

char mqtt_topic[MQTT_TOPIC_LENGTH];
char mqtt_body[MQTT_BODY_LENGTH];

char mqtt_topic_debug[MQTT_TOPIC_LENGTH];
char mqtt_body_debug[MQTT_BODY_LENGTH];

BLEScan* pBLEScan;
static BLEUUID serviceUUID(PIANO_UUID);

static bool do_connect_ble = false;
static bool connected_ble = false;
static bool do_scan = true;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* piano_device;

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
    vsnprintf(mqtt_body_debug, MQTT_BODY_LENGTH, fmt, aptr);
    va_end(aptr);

    snprintf(mqtt_topic_debug, MQTT_TOPIC_LENGTH, "%s/debug", DEVICE_ID);
    esp_mqtt_client_publish(mqtt_client, mqtt_topic_debug, mqtt_body_debug, 0, MQTT_QOS, 0);
}

class BluetoothCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) {
        connected_ble = true;
        do_scan = false;
        mqtt_send_debug("connected!");
    }

    void onDisconnect(BLEClient* pclient) {
        connected_ble = false;
        do_scan = true;
        mqtt_send_debug("disconnected :/");
    }

    /***************** New - Security handled here ********************
    ****** Note: these are the same return values as defaults ********/
    uint32_t onPassKeyRequest() {
        mqtt_send_debug("requested passkey");
        return 0;
    }

    bool onConfirmPIN(uint32_t pass_key) {
        mqtt_send_debug("The passkey YES/NO number: %d\n", pass_key);
        return true;
    }

    void onAuthenticationComplete(ble_gap_conn_desc desc) { mqtt_send_debug("holy shit it worked"); }
    /*******************************************************************/
};

class BluetoothAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice* advertisedDevice) {
        // mqtt_send_debug("found device: %s\n", advertisedDevice->toString().c_str());

        if (advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(serviceUUID)) {
            mqtt_send_debug("device is piano!");
            BLEDevice::getScan()->stop();
            piano_device = advertisedDevice;
            do_connect_ble = true;
            do_scan = false;
        }
    }
};

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length,
                           bool isNotify) {
    printf("Notify callback for characteristic %s of data length %d data: %s\n",
           pBLERemoteCharacteristic->getUUID().toString().c_str(), length, (char*)pData);
}

bool connectToServer() {
    mqtt_send_debug("Forming a connection to %s\n", piano_device->getAddress().toString().c_str());

    BLEClient* pClient = BLEDevice::createClient();
    mqtt_send_debug(" - Created client\n");

    pClient->setClientCallbacks(new BluetoothCallbacks());

    // Connect to the remove BLE Server.
    pClient->connect(piano_device);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type
                                     // of peer device address (public or private)
    mqtt_send_debug(" - Connected to server\n");

    int num_services = piano_device->getServiceDataCount();
    mqtt_send_debug("number of service UUIDs: %d", num_services);
    for (int i = 0; i < num_services; i++) {
        mqtt_send_debug("%s", piano_device->getServiceDataUUID(i).toString().c_str());
    }

    // Obtain a reference to the service we are after in the remote BLE server.
    BLERemoteService* pRemoteService = pClient->getService(piano_device->getServiceUUID(0));
    if (pRemoteService == nullptr) {
        mqtt_send_debug("Failed to find our service UUID: %s\n", serviceUUID.toString().c_str());
        pClient->disconnect();
        return false;
    }
    mqtt_send_debug(" - Found our service\n");

    /* // Obtain a reference to the characteristic in the service of the remote BLE server.
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) {
        printf("Failed to find our characteristic UUID: %s\n", charUUID.toString().c_str());
        pClient->disconnect();
        return false;
    }
    mqtt_send_debug(" - Found our characteristic\n"); */

    /* // Read the value of the characteristic.
    if (pRemoteCharacteristic->canRead()) {
        std::string value = pRemoteCharacteristic->readValue();
        mqtt_send_debug("The characteristic value was: %s\n", value.c_str());
    } */
    /** registerForNotify() has been deprecated and replaced with subscribe() / unsubscribe().
     *  Subscribe parameter defaults are: notifications=true, notifyCallback=nullptr, response=false.
     *  Unsubscribe parameter defaults are: response=false.
     */
    /* if (pRemoteCharacteristic->canNotify()) {
        // pRemoteCharacteristic->registerForNotify(notifyCallback);
        pRemoteCharacteristic->subscribe(true, notifyCallback);
    } */

    return true;
}

void connectTask(void* parameter) {
    while (true) {
        // If the flag "do_connect_ble" is true then we have scanned for and found the desired
        // BLE Server with which we wish to connect.  Now we connect to it.  Once we are
        // connected we set the connected flag to be true.
        if (do_connect_ble) {
            if (connectToServer()) {
                mqtt_send_debug("We are now connected to the BLE Server.\n");
            } else {
                mqtt_send_debug("We have failed to connect to the server; there is nothin more we will do.\n");
            }
            do_connect_ble = false;
        }

        // If we are connected to a peer BLE Server, update the characteristic each time we are reached
        // with the current time since boot.
        if (connected_ble) {
            char buf[256];
            snprintf(buf, 256, "Time since boot: %lu", (unsigned long)(esp_timer_get_time() / 1000000ULL));

            // Set the characteristic's value to be the array of bytes that is actually a string.
            /*** Note: write value now returns true if successful, false otherwise - try again or disconnect ***/
            pRemoteCharacteristic->writeValue((uint8_t*)buf, strlen(buf), false);
        } else if (do_scan) {
            mqtt_send_debug("scanning...");
            pBLEScan->start(1);  // this is just eample to start scan after disconnect, most likely there is
                                 //  better way to do it in arduino
        }

        vTaskDelay(2000 / portTICK_PERIOD_MS);  // Delay a second between loops.
    }

    vTaskDelete(NULL);
}

static void bt_init() {
    mqtt_send_debug("initializing bluetooth");
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new BluetoothAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(80);
    xTaskCreate(connectTask, "bluetooth", 10240, NULL, 2, NULL);
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
        // esp_mqtt_client_publish(mqtt_client, mqtt_topic, mqtt_body, 0, MQTT_QOS, 0);
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
