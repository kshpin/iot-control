# iot-control

My smart home IoT setup.

All actors are divided into three groups: event producers, event consumers, and the dispatcher.

Producers procure, by whatever means, event information that is sent to the dispatcher; they don't concern themselves with what that information will be used for. These modules are ESP32 microcontrollers, which receive their events through whatever peripherals are attached to them.

Consumers are told what to do by the dispatcher, without any extra deductions. These are also ESP32 microcontrollers, doing things like switching lights on and off.

The dispatcher matches events received from producers to directions to be given to consumers. A mapping is given to it that describes what event should trigger what action(s), if any. The dispatcher is a Raspberry Pi, running Python code for simplicity. What should the mapping be described by? Good question, no idea. Probably some JSON-like text file, will need to research what the best option here will be.

Here are several options for the code architecture of producers and consumers:

1. Write code for each module separately.
    1. There may be lots of unnecessary boilerplate code duplicated for different modules.
    2. Only updates to a given module and the communication with the dispatcher prompt re-flashing, as modules don't directly interact with each other.
    3. Security concerns:
        1. Compromised producers can feed malicious data.
            1. Producers self-identify with all events so the dispatcher knows how to route data, which means the dispatcher can keep a blocklist of known compromised producers and filter out events from producers on that list.
        2. Compromised consumers can be used to spy on events from producers routed to those consumers.
            1. Compromised consumers can also be placed on a blocklist, and outgoing events filtered through it.
2. Write one program to be uploaded to all modules, with runtime identification of what configuration to run it in.
    1. Find a way to upload the program to the module with one constant changed for each module, to uniquely identify which module it is. The module will then contact the dispatcher on startup with its identifier, and the dispatcher will return the configuration the module is to use for the duration of its uptime.
    2. Updates to the shared module code _may_ prompt a re-flashing for all modules, since one of the main benefits of this approach is that any module can fulfill any role based on the dispatcher's response, and an update to a part of the code not currently in use by some module will render that module outdated with respect to that code.
    3. This option might run into constraints with regards to the firmware size - the shared module code may get too large for microcontrollers (is this likely?).
    4. Security concerns:
        1. All modules carry all code on them, so in the case that a compromised module's code can be disassembled, all modules' possible functionality becomes exposed.
