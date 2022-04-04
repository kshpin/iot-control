# iot-control

My smart home IOT setup.

All actors are divided into three groups: event producers, event consumers, and the dispatcher.

Producers procure, by whatever means, event information that is sent to the dispatcher; they don't concern themselves with what that information will be used for. These modules are ESP32 microcontrollers, which receive their events through whatever peripherals are attached to them.

Consumers are told what to do by the dispatcher, without any extra deductions. These are also ESP32 microcontrollers, doing things like switching lights on and off.

The dispatcher matches events received from producers to directions to be given to consumers. A mapping is given to it that describes what event should trigger what action(s), if any. The dispatcher is a Raspberry Pi, running Python code for simplicity. What should the mapping be described by? Good question, no idea. Probably some JSON-like text file, will need to research what the best option here will be.
