# **ESPHome-Chinbasto**

A custom ESPHome component for controlling Chinese Webasto heaters using ESP32. This project adds **on/off control** via soldering, as the serial communication is currently read-only. It is based on the work of [timmchugh11](https://github.com/timmchugh11/Chinese-Diesel-Heater---ESPHome) and builds on the reverse engineering efforts of **Ray Jones**.

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

---

## **Table of Contents**
1. [Introduction](#introduction)
2. [Features](#features)
3. [Hardware Requirements](#hardware-requirements)
4. [Installation](#installation)
5. [Configuration](#configuration)
6. [Usage](#usage)
7. [Soldering to On/Off Switch](#soldering-to-onoff-switch)
8. [Documentation](#documentation)
9. [Credits](#credits)
10. [License](#license)

---

## **Introduction**
This project enables control of Chinese Webasto heaters using ESP32 and ESPHome. It extends the functionality of the original work by [timmchugh11](https://github.com/timmchugh11/Chinese-Diesel-Heater---ESPHome) and incorporates insights from **Ray Jones**' reverse engineering of the serial communication protocol.

The current implementation focuses on **read-only serial communication** and adds **on/off control** via GPIO pins, requiring soldering to the heater's control board.

---

## **Features**
- **Read-only serial communication**: Monitor heater status, temperature, fan speed, and error codes.
- **On/Off control**: Turn the heater on/off via GPIO pins.
- **Smart On/Off**: Automatically handles 1-second or 3-second button presses based on the heater's current state.
- **MQTT integration**: Publish heater data to an MQTT broker.
- **OTA updates**: Update the firmware over-the-air.
- **Web interface**: Access a local web interface for monitoring and control.

---

## **Hardware Requirements**
- **ESP32 development board** (e.g., ESP32 DevKit).
- **Chinese Webasto heater** with a compatible control board: "3-wire": the 3 wires: red,blue and black connect to webasto controller via "triangle" connector
- **Soldering tools**: To connect GPIO pins for on/off control.
- **1k resistors**: For connecting GPIO pins to the heater's control board.
- **Wiring**: Connect the ESP32 to the heater's serial interface and GPIO pins.

---

## **Installation**
1. **Clone the repository**:
   ```bash
   git clone https://github.com/pavelw/esphome-chinbasto.git
   cd esphome-chinbasto
   ```

2. **Install ESPHome**:
   - Follow the [ESPHome installation guide](https://esphome.io/guides/getting_started_command_line.html) to set up ESPHome on your system.

3. **Set up `secrets.yaml`**:
   - Copy `secrets.yaml.template` to `secrets.yaml` and fill in your Wi-Fi credentials, MQTT broker details, and OTA password.

4. **Compile and upload**:
   - Compile the firmware and upload it to your ESP32:
     ```bash
     esphome run esp32-webasto36.yaml
     ```

---

## **Configuration**
### **YAML Configuration**
The main configuration file is `esp32-webasto36.yaml`. Key sections include:
- **Wi-Fi**: Configure your Wi-Fi network.
- **MQTT**: Set up MQTT integration for remote monitoring.
- **UART**: Configure serial communication with the heater.
- **GPIO**: Define pins for on/off control.

### **Custom Component**
The custom component (`heater.h`) handles serial communication and GPIO control. Work done by [timmchugh11](https://github.com/timmchugh11/Chinese-Diesel-Heater---ESPHome).
It is based on the reverse-engineered protocol documented by **Ray Jones**. 

---

## **Usage**
1. **Monitor heater status**:
   - Use the ESPHome web interface or MQTT to monitor the heater's status, temperature, fan speed, and error codes.

2. **Control the heater**:
   - Use the web interface or MQTT commands to turn the heater on/off.
   - The **Smart On/Off** feature automatically handles 1-second or 3-second button presses based on the heater's current state.

3. **Debugging**:
   - Check the ESPHome logs for debugging information.

---

## **Soldering to On/Off Switch**
To add **on/off control** to your heater, you need to solder connections to the heater's control board. Here's how:

### **Connecting GPIO Pins**
1. **Identify the Power Button Pins**:
   - Locate the pins for the **Power** button on the heater's control board. You may also identify pins for **Up** and **Down** buttons if you want to control the duty cycle.

2. **Add 1k Resistors**:
   - Connect the ESP32's GPIO pins to the heater's control board using **1k resistors** to protect the circuitry.

3. **Wire the Connections**:
   - **Power Button**: Connect a GPIO pin (e.g., GPIO13) to the Power button pin via a 1k resistor.
   - **Up/Down Buttons** (optional): Connect additional GPIO pins to the Up and Down button pins via 1k resistors.

4. **Configure GPIO in ESPHome**:
   - In the `esp32-webasto36.yaml` file, configure the GPIO pins for the Power button and optional Up/Down buttons:
     ```yaml
     switch:
       - platform: gpio
         name: "Power (debug: switches transistor in the button)"
         id: chinbasto_power
         pin: GPIO13
         inverted: false
         restore_mode: ALWAYS_OFF
     ```

### **Smart On/Off Control**
The **Smart On/Off** feature in the YAML configuration automatically handles the required button press duration:
- **1-second press**: Turns the heater on.
- **3-second press**: Turns the heater off.

This is implemented in the `button` section of the YAML file:
```yaml
button: 
  - platform: template
    name: "Power ON (smart)"
    id: button_on
    on_press:
      then:
        - switch.turn_on: chinbasto_power
        - delay: 1s
        - logger.log: "button: trying to put chinbasto ON"
        - switch.turn_off: chinbasto_power

  - platform: template
    name: "Power OFF (smart)"
    id: button_off
    on_press:
      then:
        - switch.turn_on: chinbasto_power
        - delay: 3s
        - logger.log: "button: trying to put chinbasto OFF (long press)"
        - switch.turn_off: chinbasto_power
```

### **Transistor Pin Identification**
When soldering to the control board, you may need to identify the **emitter, base, and collector** of a transistor. A helpful video tutorial can be found here:  
[How to Identify Emitter, Base, and Collector on a Transistor](https://www.youtube.com/watch?v=W5yfAZoE5sY)

---

## **Documentation**
### **Serial Protocol**
The serial communication protocol for Chinese Webasto heaters was reverse-engineered by **Ray Jones**. Detailed documentation can be found in his work:
- [V9 - Hacking the Chinese Diesel Heater Communications Protocol](https://gitlab.com/mrjones.id.au/bluetoothheater/-/blob/master/Documentation/V9%20-%20Hacking%20the%20Chinese%20Diesel%20Heater%20Communications%20Protocol.pdf?ref_type=heads)

### **ESPHome Configuration**
- Refer to the [ESPHome documentation](https://esphome.io/) for details on configuring ESPHome components.

---

## **Credits**
This project builds on the work of:
- **[timmchugh11](https://github.com/timmchugh11/Chinese-Diesel-Heater---ESPHome)**: Original ESPHome integration for Chinese diesel heaters.
- **[Ray Jones](https://gitlab.com/mrjones.id.au/bluetoothheater/-/blob/master/Documentation/V9%20-%20Hacking%20the%20Chinese%20Diesel%20Heater%20Communications%20Protocol.pdf?ref_type=heads)**: Reverse engineering of the serial communication protocol.

---

## **License**
This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for details.