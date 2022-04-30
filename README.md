# iot-control

My smart home IoT setup.

## Overview

All actors are divided into three groups: event producers, event consumers, and the dispatcher.

Producers procure, by whatever means, event information that is sent to the dispatcher; they don't concern themselves with what that information will be used for. These modules are ESP32 microcontrollers, or possibly STM32, which receive their events through whatever peripherals are attached to them.

Consumers are told what to do by the dispatcher, without any extra deductions. These are also ESP32/STM32 microcontrollers, doing things like switching lights on and off.

The dispatcher matches events received from producers to directions to be given to consumers. A mapping is given to it that describes what event should trigger what action(s), if any. Events are characterized by their origin producer identifier, and any other information the producer transmits; for instance, device `piano` connected to a piano keyboard will report to the dispatcher every time a key is pressed or released, and the report might look something like this:

```
Topic: "producers/piano/key"
Body: "60 up" (key with MIDI id 60, middle C, released)
```

The dispatcher is a Raspberry Pi, running an MQTT server, and a [Node-RED][nodered] server for routing between producers and consumers.

## Communication protocol thoughts

1. UDP
    1. Simplest in principle.
    2. Potentially lossy.
    3. Producers would need to identify themselves with each message for proper routing.
2. MQTT
    1. Keeps state of last message sent, so that consumers can reboot and keep state, if each message carries information about the target state absolutely, rather than relative to the current state.
    2. Can send a will for killed devices, to let the dispatcher know that that device is no longer up.
    3. Technically has a security option... but see general security concerns.

MQTT is pretty clearly a winner here.

## Architecture thoughts

1. Write code for each module separately.
    1. There may be lots of unnecessary boilerplate code duplicated for different modules.
    2. Only updates to a given module and the communication with the dispatcher prompt re-flashing, as modules don't directly interact with each other.
    3. Security concerns:
        1. I don't know if there's anything specific to this approach, see general security concerns.
2. Write one program to be uploaded to all modules, with runtime identification of what configuration to run it in.
    1. Find a way to upload the program to the module with one constant changed for each module, to uniquely identify which module it is. The module will then contact the dispatcher on startup with its identifier, and the dispatcher will return the configuration the module is to use for the duration of its uptime.
    2. Updates to the shared module code _may_ prompt a re-flashing for all modules, since one of the main benefits of this approach is that any module can fulfill any role based on the dispatcher's response, and an update to a part of the code not currently in use by some module will render that module outdated with respect to that code.
    3. This option might run into constraints with regards to the firmware size - the shared module code may get too large for microcontrollers (is this likely?).
    4. Security concerns:
        1. All modules carry all code on them, so in the case that a compromised module's code can be disassembled, all modules' possible functionality becomes exposed.

Writing code for each module separately is the option I chose to go with, it's not as clear which one is better here to me but the first option is less hassle in terms of re-flashing old modules that don't need to change, especially in an environment where revisions of past modules probably won't be a common occurrence.

## General security concerns

1. Compromised producers can feed malicious data.
    1. Producers can self-identify with all events so the dispatcher knows how to route data, which means the dispatcher can keep a blocklist of known compromised producers and filter out events from producers on that list.
        1. With Node-RED, this will look more like just not subscribing to topics that that device emits.
2. Compromised consumers can be used to spy on events from producers routed to those consumers.
    1. Compromised consumers can also be placed on a blocklist, and outgoing events filtered through it.
        1. With Node-RED, route the events that would go to those consumers to other consumers instead, or no consumers at all temporarily, while replacements are not in place yet.
3. Network traffic can be snooped on by other agents on the network.
    1. Keep a separate LAN specifically for the devices in this project ([DMZ][dmz]), and expose only the dispatcher for configurations.
        1. The dispatcher itself will then need to be protected, on top of a password ideally also only allowing connections from one IP on the local network, which will be the configuring node.
        2. This actually removes the need for securing the MQTT communication - it's secured already, one layer lower, and this way there's less hassle of setting up the microcontrollers to know keys and such.
    2. On startup, producers and consumers can give the dispatcher their individual public keys, and the dispatcher can respond with its own public key, so that each pair of communicating devices is unintelligible to the rest.
        1. Producers and consumers can generate their public/private key pairs every time on startup, so that a compromised private key is only usable for as long as the dispatcher thinks that device is running.
4. A malicious agent can create their own producer device mimicking as a valid one, running on the network and feeding malicious events.
    1. The malicious agent would need to know the network SSID, network password, IP of the dispatcher, ID of a device that is recognized by the dispatcher's routing server, and the event description format of that device. The hardest information to get here is the network information and device ID, which normally should be inaccessible as it is (but still possible of course).
    2. The network can be set up to allow specified MAC addresses, which would be those of the desired devices.
        1. The malicious agent could set their device's MAC address to be the same as that of the valid device (another piece of information they'd need), in which case... I'm not sure what can even be done at this stage to be honest.

[nodered]: https://nodered.org/
[dmz]: https://en.wikipedia.org/wiki/DMZ_(computing)
