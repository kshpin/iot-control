#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "../include/config.h"

#define LED_BUILTIN 1

#define BLINK true
#define MONITOR false

bool connected = false;

WiFiUDP udp;

void on_wifi_data(WiFiEvent_t event) {
    switch (event) {
        case SYSTEM_EVENT_STA_GOT_IP:
            // When connected set
            Serial.print("WiFi connected! IP address: ");
            Serial.println(WiFi.localIP());

            // initializes the UDP state
            // This initializes the transfer buffer
            udp.begin(WiFi.localIP(), udp_port);
            connected = true;
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            Serial.println("WiFi lost connection");
            connected = false;
            break;

        default:
            break;
    }
}

void connect_to_wifi(const char *ssid, const char *password) {
    WiFi.disconnect(true);
    WiFi.onEvent(on_wifi_data);
    WiFi.begin(ssid, password);
}

void setup() {
    // https://savjee.be/2020/01/multitasking-esp32-arduino-freertos/

    if (BLINK) pinMode(LED_BUILTIN, OUTPUT);
    if (MONITOR) Serial.begin(115200);

    connect_to_wifi(ssid, password);
}

unsigned long last_blink = 0;
bool on = false;

void loop() {
    unsigned long cur_millis = millis();
    if (cur_millis - last_blink > 500) {
        last_blink = cur_millis;

        if (connected) {
            udp.beginPacket(target_address, udp_port);
            udp.printf("ms since boot: %lu\n", cur_millis);
            udp.endPacket();
        }

        if (BLINK) {
            if (on) {
                digitalWrite(LED_BUILTIN, LOW);
            } else {
                digitalWrite(LED_BUILTIN, HIGH);
            }
        }

        on = !on;
    }
}
