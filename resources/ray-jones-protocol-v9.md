<!--
Text copy of "Ray Jones - Hacking the Chinese Diesel Heater Communications  
Protocol V9.pdf" (same folder), for search and quoting. Converted  
2026-11-13 from the PDF's glyph codes, without extra tools. The two byte  
tables, the capture table and the pump formula were rebuilt by hand.  
Figures, schematics and charts are in the PDF only. Where this copy and the  
PDF differ, the PDF is right.  
Original: https://gitlab.com/mrjones.id.au/bluetoothheater  
-->

# Hacking the Chinese Diesel Heater Communications Protocol
Ray Jones, Rev 9, 21 Dec 2018.  
ray@mrjones.id.au  
## Introduction
I recently installed a "Chinese diesel heater" into our caravan.  
It was originally supplied with the basic Rotary Controller, which was then upgraded  
to an LCD Controller once I learnt the LCD Controller allows you to control mixture  
settings via a protected settings menu.  
I wanted to tune the mixture as my heater would start screaming at full power, ala a  
real case of "full noise" !  
It really was very noisy, and very objectional, 10"s of metres away. Screamed like a  
banshee.  
Delicate crimping of the fuel line with pliers revealed this was probably due to too  
much fuel.  
Whilst the LCD is cool n groovy and did indeed help me to tame the noise, it has the  
distinct annoyance that the display is always on. ALWAYS.  
Firstly, this unnecessarily wastes power, especially when switched off, and could be a  
concern when 100% reliant upon solar recharge in bush camp settings.  
Others have complained the LCD is simply too bright when shut eye time comes,  
especially as it remains lit even when the heater is off!  
So, there is a mystical blue wire that connects between the heater itself and the  
controller.  
If one can work out the serial protocol that exists upon that wire, one could roll their  
own controller implementation…  

<!-- page 2 -->

This has the distinct potential to improve the usability for "those that just have to  
fiddle" , lower power consumption, even develop friendly user interfaces that could  
be hosted via a smart phone app using Bluetooth!  
## Hardware Under Investigation
To set the scene of what is being discussed here, the units I have access to are the  
basic Rotary Controller, common to many ebay.au listings, as well as the LCD and LED  
variants I later bought.  
I also obtained a blue LCD a few months later.  
The plug on my loom is a triangular 3 pin water proof connector. The triangular plug  
is not easy to find/source .  
I note that the plug style may vary on other brands, my blue LCD came with a round  
plug initially which I changed to a triangular style  
The relevance of this information may, or may not, apply to those other units.  
The microcontroller device in the Rotary Controller is an STM8S003F3P6, in a TSSOP20  
package.  
The LCD Controller I suspect is in the same family, but it is in a LQFP32 package.  
The manufacturer has ground the top off to hide the device ID, but the Tx/Rx pins do  
agree with the data sheet!  
The LED controller has not been disassembled, but the same processor family is  
suspected.  
The blue LCD has a much higher pin count CPU, and is unmarked – still may be an STM  
device…  

<!-- page 3 -->

## Power Supply
5V is supplied via the red and black wires; red +5V, black 0V.  
The controllers do not include a voltage regulator or reverse polarity protection.  
Connecting 12V will destroy them – BE WARNED!  
## Half-Duplex Serial Data
Half duplex serial data as 8N1 UART encoding travels over the blue wire.  
The electronics to route the transmit and receive signals is quite unusual.  
It uses a pair of BJT transistors to connect the Tx and Rx pins of the micro to the  
single blue wire, over which signalling can only happen one way at a time.  
An NPN transistor exists on the Tx data path, a PNP transistor on the Rx data path.  
A "TxENB"gating signal determines which of these transistors is able to turn on, only  
allowing the serial data signal to traverse that path.  
"TxENB"is a GPIO signal and must be high for the micro to send data, with reference  
to the following schematic:  
"TxENB"high:  
T1 can conduct due to correct Base Emitter biasing. T2 is held off.  
The signal from the micro"s Tx output pin drives the blue wire.  
The micro"s Rx input pin is always high.  
"TxENB"low:  
"TxENB"is low for most of the time.  
T1 is now held off, but T2 can now conduct as its Base Emitter biasing now  
allows it.  
The micro"s Rx input follows the blue wire.  
Note that when Tx data is sent, this does not appear back on the Rx data pin!  
Unusual for sure, but it does model OK in LT-Spice.  
It also obviously works in real life.  
The part numbers BC817 & BC807 were found by Googling "6C" and "5CT" as the SMD  
body markings. These have been confirmed as correct due to an unfortunate incident  
requiring the transistors to be replaced in my LCD Controller  
The half-duplex serial circuit is as follows:  

<!-- page 4 -->

An example of the signal timing follows.  
Note the controller sends data (Tx), then the heater responds about 5ms later (Rx).  
Also note the gating signal on the top line that allows the clean split of Rx from Tx, on  
the micro"s Rx input due the above circuitry.  

<!-- page 5 -->

## Temperature Sensing
A small beaded thermistor sits near the edge of the PCB, acting as a temperature  
sensor.  
Empirical testing reveals the following results:  
Open circuit thermistor: LCD shows -39 °C  
Shorted thermistor: LCD shows 50 °C  
With the thermistor isolated, a resistance of 128k Ω was measured at 20 °C,  
dropping with increased temperature.  
Based upon the circuitry and the above test results, the thermistor is suspected to be  
a common glass beaded 100k Ω NTC type.  
The newer blue LCD uses a BMP180, a combined temperature & barometric sensor,  
connected via I2C.  
The BMP180"s accuracy is specified as ±2 °C which is terrible really!  

<!-- page 6 -->

## Data Protocol
The minimum bit period is 40us.  
This is a somewhat oddball baud rate of 25,000 bits per second.  
No big deal, just unusual.  
The pulse pattern is conventional Asynchronous Serial type signalling with start and  
stop bits appearing to exist in the right places.  
An Intronix Logicport logic analyser was configured to sample the Tx data line coming  
directly from the actual microcontroller.  
An Asynchronous Serial interpreter was configured:  
Baud Rate 25000  
Logic Sense positive  
Structure 8N1, LSB first  
Display Format Hexadecimal  
This revealed there were 24 bytes being sent in a burst, all neatly packed directly  
adjacent to each other, regularly repeating.  
The repeat rate varies, dependent upon the controller:  
Rotary Controller: ~300ms  
LCD Controller: ~800ms  
LED Controller: ~300ms  
Note that if the controller is not connected to the heater, the heater unit does not  
send any data.  
i.e. all transmissions from the heater are in direct response to a controller  
transmission.  
24 bytes is a fair bit of info, so I tried pressing the on/off buttons to see what  
happened.  
The most obvious changes were the last two bytes always changed, which suggests a  
16bit CRC lives there.  
Closer inspection showed that certain bytes would change according to what was done.  
Everything was clearly binary values, no ASCII data.  
All very interesting, but the Rotary Controller was proving quite limited in exercising  
the link, so I swapped to the LCD Controller.  
Now real progress was made.  
Pressing the up/down buttons to change the demanded temperature causes the 5th  
byte to change value up/down by 1.  
Converting the hexadecimal value to decimal soon revealed the binary value was  
exactly the desired temperature, in Celsius, as you"d see on the display – Ah ha!  
I eventually configured a second interpreter to read decimal values in parallel.  

<!-- page 7 -->

Resting a finger on the thermistor (temperature sensor) showed the byte 4"s value  
change upwards!  
Eventually by pushing this, doing that, going into the protected settings most of the  
mystical bytes started to reveal that they were simply conveying pretty much the all  
values presented in the LCD to the heater base unit on a continual basis.  
But, not all locations are fully explained just yet .  
This is a typical capture from my Rotary Controller shortly after being powered up.  
After a few seconds 7 LEDs are flashing about the knob – the blue wire goes nowhere,  
E-07!  
Despite that, it still spews out a data stream like below, repeating about every 300ms.  
It is always 24 bytes of binary data, only the contents change.  
Whilst binary in general sucks as it is not self-describing, life is in some ways easier as  
you do not need to discover potentially hidden ASCII messages etc.  
An Arduino sketch was later developed, and is provided later, which allows sniffing of  
the blue wire; collecting the two-way conversation taking place between the  
controller and heater.  
The sketch is intended to be used in conjunction with a serial terminal application on  
a PC, logging the data received from the Arduino, over USB.  
Putty has been used with success here to create log files which are later analysed,  
primarily using Excel.  

<!-- page 8 -->

## Data Protocol Byte Definitions
### Transmit Packet
The following table reveals what has been determined for the 24 message bytes  
coming from the controller, to the heater .  
Bytes I"m not 100% certain on are italicised in the Purpose column.  
| Index | Purpose | Known values | Scaling |
|---|---|---|---|
| 0 | Start of frame? | 78h, 76h (Rotary, LCD) | - |
| 1 | Data size? | 16h (constant) | - |
| 2 | Command | 00h / 05h (stop) / A0h (start) | - |
| 3 | Temp sensor | variable, 0 in fixed mode | 1 °C / digit |
| 4 | Desired temperature | variable | 1 °C / digit |
| 5 | Minimum pump frequency | variable | 0.1 Hz / digit *** |
| 6 | Maximum pump frequency | variable | 0.1 Hz / digit *** |
| 7, 8 | Minimum fan speed (MSB, LSB) | 16 bit variable | 1 RPM / digit *** |
| 9, 10 | Maximum fan speed (MSB, LSB) | 16 bit variable | 1 RPM / digit *** |
| 11 | Heater operating voltage | 78h, F0h (12.0 V / 24.0 V) | 0.1 V / digit *** |
| 12 | Fan speed sensor | 01h, 02h (FN-1, FN-2) | - |
| 13 | Thermostat / fixed mode | 32h, CDh (thermostat, fixed) | byte 4 = 0 when fixed |
| 14 | Lower temperature limit | 08h (constant) | 1 °C / digit |
| 15 | Upper temperature limit | 23h (constant) | 1 °C / digit |
| 16 | Glow plug power | 05h | 1-6 |
| 17 | Manual pump (fuel prime) | 00h (normal), 5Ah (prime) | - |
| 18, 19 | Unknown (MSB, LSB) | 01h 2Ch (old), EBh 47h (blue LCD) | ? |
| 20, 21 | Altitude (MSB, LSB) | 0DACh = 3500 (old), variable (blue LCD) | 1 m / digit |
| 22, 23 | CRC-16/MODBUS (MSB, LSB) | 16 bit variable | - |

*** Tx Bytes[5..11] (Shaded orange) are ineffective from a Rotary Controller.  
A more detailed discussion of the message bytes follows:  

<!-- page 9 -->

#### Tx Byte[0] - Start of Frame?
Always 78h from Rotary Controller – bytes [5..11] are ignored.  
Always 76h from LCD or LED Controller – bytes [5-11] are accepted .  
The value of this byte appears to have control over what parameters the heater unit  
will actually listen to, and also save into its EEPROM.  
The LCD/LED menus can tune the max min pump and fan speeds (mixture settings)  
and these also appear to be retained in the heater micro"s internal EEPROM.  
When you plug a Rotary controller in, its advertised max/min pump & fan settings are  
ignored, but the running limits match the max/min values that were set with an  
LCD/LED!  
Likewise the heater ignores the operating voltage from a Rotary Controller, but  
honours it and faults out with voltage errors if set wrong from an LCD/LED.  
#### Tx Byte[1] - Data size?
Always 16h => 22 decimal.  
Adding 22 + 2 CRC bytes = 24 bytes…  
#### Tx Byte[2] - Heater On/Off control
This byte is usually sent as 00h (do nothing / no change).  
Others values are only sent at the instant the user desires to heater to start or stop:  
To start the heater, send A0h (for one transmission)  
– confirmation is relayed back in the Rx Data when "Run State" goes to 01h.  
To stop the heater, send 05h (for one transmission)  
– confirmation is relayed back in the Rx Data when "Run State" goes to 7, "Post  
glow" .  
#### Tx Byte[3] - Current actual temperature in degrees Celsius.
Changes if you heat/cool the bead thermistor on the PCB"s edge (device that sticks  
out of the case"s hole).  
Each digit scales as 1 °C, as read in decimal.  
#### Tx Byte[4] - Desired temperature in degrees Celsius.
The streamed value exactly follows the LCD/LED"s value after conversion to decimal.  
Each digit scales as 1 °C, as read in decimal.  

<!-- page 10 -->

#### Tx Byte[5] - Minimum Pump Frequency.
The minimum pump speed as set in the protected settings of the LCD/LED.  
Each digit scales as 0.1Hz, as read in decimal => 10h = 16 decimal = 1.6Hz.  
Combines with the minimum fan speed to define the low end fuel mixture.  
*** Ineffective from the Rotary Controller.  
#### Tx Byte[6] - Maximum Pump Frequency.
The maximum pump speed as set in the protected settings of the LCD/LED.  
Each digit scales as 0.1Hz, as read in decimal => 37h = 55 decimal = 5.5Hz.  
Combines with the maximum fan speed to define the high end fuel mixture.  
*** Ineffective from the Rotary Controller.  
#### Tx Bytes[7..8] - Minimum Fan Speed.
Combined as a 16bit value, Tx Byte[7] is the MSB.  
The minimum fan speed as set in the protected settings of the LCD/LED.  
Each digit scales as 1RPM as read in decimal => 0690h = 1680 decimal = 1680RPM.  
Combines with the minimum pump frequency to define the low end fuel mixture.  
*** Ineffective from the Rotary Controller.  
#### Tx Bytes[9..10] - Maximum Fan Speed.
Combined as a 16bit value, Tx Byte[9] is the MSB.  
The maximum fan speed as set in the protected settings of the LCD/LED.  
Each digit scales as 1RPM, as read in decimal => 1194h = 4500 decimal = 4500RPM.  
Combines with the maximum pump frequency to define the high end fuel mixture.  
*** Ineffective from the Rotary Controller.  
#### Tx Byte[11] - Heater Operating voltage.
Follows the operating voltage selection in the protected settings of the LCD/LED.  
Each digit scales as 0.1V, as read in decimal.  
Can be 78h or F0h (120 or 240 decimal => 12.0 or 24.0V)  
If wrong from an LCD/LED, voltage faults will be generated, eg E-01 when set to 24V  
with a 12V power supply.  
*** Ineffective from the Rotary Controller.  
#### Tx Byte[12] - Fan Speed Sensor count selection.
01h or 02h.  
Follows the fan speed sensor selection in the protected settings of the LCD/LED. (SN-1  
or SN-2).  
#### Tx Byte[13] - Thermostatic or fixed power mode.
Toggling the operating mode on the controller changes this byte"s value:  
32h is Thermostatic mode.  
CDh is Fixed power Hz mode. (one"s complement of 0x32!)  

<!-- page 11 -->

When in Fixed power Hz mode, Tx Byte 4 always gets forced to zero (no temperature  
feedback).  
#### Tx Byte[14] - Minimum allowed temperature setting.
Always 08h.  
Matches the minimum desired temperature the controller can be set to in byte #5  
(8 °C).  
#### Tx Byte[15] - Maximum allowed temperature setting.
Always 23h.  
Matches the maximum desired temperature the controller can be set to in byte #5  
(35 °C).  
#### Tx Byte[16] - Requested glow plug power?
This is always 05h on the older controllers.  
The new Blue (and presumably gold) controllers have a "PF" setting in the protected  
menus.  
This byte"s value follows whatever PF is set to.  
Some heater manuals state this is glow plug power, but changing this value has no  
effect on my heater – there may also be a firmware update in the later model heaters  
than do react to this byte. It can be varied from 1-6, allegedly 35W, 40W, 45W, 80W,  
85W and 90W respectively.  
#### Tx Byte[17] - Manual pump mode (priming).
00h in normal mode.  
5Ah when in manual pump priming mode.  
#### Tx Bytes[18..19] - Unknown
Always 012Ch = 300 decimal with old LCD controllers?  
Always EB47h with new blue LCD.  
#### Tx Bytes[20..21] - Altitude in metres
Combined as a 16bit variable, MSB in Tx Byte[20].  
Always 0DACh (3500) on older controllers without a barometer.  
The blue LCD hosts a BMP180 which is a combined temperature & barometric sensor.  
#### Tx Bytes[22..23] - CRC-16 / MODBUS checksum.
Calculated from the first 22 bytes.  
Combined as a 16bit value, Tx Byte[22] is the MSB.  

<!-- page 12 -->

### Receive Packet
The following table reveals what has been determined for the 24 message bytes  
coming from the heater, into the controller :  
| Index | Purpose | Known values | Scaling |
|---|---|---|---|
| 0 | Start of frame? | 76h (constant) | - |
| 1 | Data size? | 16h (constant) | - |
| 2 | Run state | 0, 1, 2, 3, 4, 5, 7, 8 | - |
| 3 | On / off | 0, 1 | - |
| 4, 5 | Supply voltage (MSB, LSB) | 16 bit variable | 0.1 V / digit |
| 6, 7 | Fan RPM (MSB, LSB) | 16 bit variable (within user limits) | 1 RPM / digit |
| 8, 9 | Fan voltage (MSB, LSB) | 16 bit variable | 0.1 V / digit |
| 10, 11 | Heat exchanger temperature (MSB, LSB) | 16 bit variable | 1 °C / digit |
| 12, 13 | Glow plug voltage (MSB, LSB) | 16 bit variable | 0.1 V / digit |
| 14, 15 | Glow plug current (MSB, LSB) | 16 bit variable | 10 mA / digit |
| 16 | Pump frequency, actual | variable (within user limits) | 0.1 Hz / digit |
| 17 | Error code | variable | - |
| 18 | Unknown | 00h (constant) | - |
| 19 | Fixed mode pump frequency | variable | 0.1 Hz / digit |
| 20 | Run mode transition temp? | 64h (constant) | 1 °C / digit |
| 21 | End of frame? | 00h (constant) | - |
| 22, 23 | CRC-16/MODBUS (MSB, LSB) | 16 bit variable | - |

<!-- page 13 -->

#### Rx Byte[0] - Start of Frame?
Always 0x76h.  
#### Rx Byte[1] - Data size?
Always 16h => 22 decimal.  
Adding 22 + 2 CRC bytes = 24 bytes…  
#### Rx Byte[2] - Run State.
Conveys the state the heater is operating at:  
0. Off / Standby  
1. Start Acknowledge  
2. Glow plug pre-heat  
3. Failed ignition - pausing for retry.  
4. Ignited – heating to full temp phase.  
5. Running.  
6. Skipped – stop acknowledge?  
7. Stopping - Post run glow re-heat  
8. Cooldown – returns to state 0 when body temp < 55 °C  

<!-- page 14 -->

#### Rx Byte[3] - Error State.
00h if the heater is idle  
01h if the heater is running normally (E-00).  
02h+ - follows the value of Rx Byte[17] when error conditions occur.  
Note the value here is always 1 more than the displayed error code.  
eg: a simulated pump over current fault shows 5 here, the controller shows E-04.  
#### Rx Bytes[4..5] - Measured supply voltage.
Combined as a 16bit variable, MSB in Rx Byte[4].  
Reports voltage at heater as 0.1V / digit.  
Based upon resistor scaling in heater, will show up to around 32V maximum.  
NOTE: this reads ~0.6V low due to protection diode within heater!  
#### Rx Bytes[6..7] - Fan RPM.
Combined as a 16bit variable, MSB in Rx Byte[6].  
Reports fan speed as 1 RPM / digit.  
#### Rx Bytes[8..9] - Fan Voltage
Combined as a 16bit variable, MSB in Rx Byte[8].  
Reports average voltage across the fan motor as 0.1V / digit.  
Note: when the heater is not running, this value tends to match the supply voltage  
value.  
This must be due to the topology of the sensing circuit.  
#### Rx Bytes[10..11] - Heat exchanger body temperature.
Combined as a 16bit variable, MSB in Rx Byte[10].  
Reports temperature of heat exchanger body as 1 °C / digit.  
#### Rx Bytes[12..13] - Glow plug voltage.
Combined as a 16bit variable, MSB in Rx Byte[12].  
Reports voltage across glow plug as 0.1V / digit.  
#### Rx Bytes[14..15] - Glow plug current.
Combined as a 16bit variable, MSB in Rx Byte[14].  
Reports current applied to glow plug as 10mA / digit.  
#### Rx Byte[16] - Pump frequency, actual.
Reports current rate of fuel pump pulses.  
Scaled as 0.1Hz / digit.  

<!-- page 15 -->

#### Rx Byte[17] - Stored Error Code from heater.
The value shown here is +1 over the displayed error on a controller.  
eg 5 here shows E-04 on a controller.  
0. No Error  
1. No Error, but started  
2. Voltage too low  
3. Voltage too high  
4. Ignition plug failure (no measured current flow?)  
5. Pump Failure – over current  
6. Too hot  
7. Motor Failure  
8. Serial connection lost  
9. Fire is extinguished  
10. Temperature sensor failure  
#### Rx Byte[18] - Unknown .
Always 00h.  
#### Rx Byte[19] - Fixed Mode Pump Frequency.
Changes to desired temperature appear to be integrated over about 8 seconds within  
the heater.  
The final pump frequency changes according to the following formula:  
```
Fdesired = Fmin + (Tdes - Tmin) / (Tmax - Tmin) x (Fmax - Fmin)
```

Where: Fdesired = Rx Byte[19], Fmin = Tx Byte[5], Fmax = Tx Byte[6],  
Tdes = Tx Byte[4], Tmin = Tx Byte[14], Tmax = Tx Byte[15].  

The LCD/LED controller reads this byte to display the desired Hz value.  
Desired Hz is not sent by the controller directly, it is calculated as shown above and  
fed back!  
This value is scaled as 0.1Hz / digit.  
#### Rx Byte[20] - Run mode transition temperature?.
Always 64h = 100.  
The heater controller appears to usually transition from Run State 4 to 5 when the  
heat exchanger reaches 100 degrees... Specualtion!  

<!-- page 16 -->

#### Rx Byte[21] - Unknown
Always 00h.  
#### Rx Bytes[22..23] - CRC-16 / MODBUS checksum.
Combined as a 16bit variable, MSB in Rx Byte[22].  

<!-- page 17 -->

## CRC-16 Decoding
| Event | Bytes 1 to 24 |
|---|---|
| Power On | `78 16 00 16 23 10 37 06 90 11 94 F0 01 32 08 23 05 00 01 2C 0D AC DC ED` |
| 10 secs | `78 16 00 17 23 10 37 06 90 11 94 F0 01 32 08 23 05 00 01 2C 0D AC 20 D0` |
| Off | `78 16 05 17 23 10 37 06 90 11 94 F0 01 32 08 23 05 00 01 2C 0D AC 75 1C` |
| Long Off | `78 16 05 17 23 10 37 06 90 11 94 F0 01 32 08 23 05 5A 01 2C 0D AC 78 44` |
| Long On | `78 16 05 00 23 10 37 06 90 11 94 F0 01 CD 08 23 05 00 01 2C 0D AC 36 28` |
| Min temp | `78 16 05 00 08 10 37 06 90 11 94 F0 01 CD 08 23 05 5A 01 2C 0D AC D7 09` |
| Long on | `78 16 05 18 23 10 37 06 90 11 94 F0 01 32 08 23 05 5A 01 2C 0D AC 0D 13` |
| Off | `78 16 05 00 23 10 37 06 90 11 94 F0 01 CD 08 23 05 00 01 2C 0D AC 36 28` |

The above table shows some of the initial byte sequences that were captured from a  
Rotary Controller.  
The shaded cells indicate the values that were different to the previous line.  
Note the last 2 bytes always change value if another value changed, this is classic CRC  
behavior.  
Being a 16bit CRC, there are many and varied implementations out there.  
Fortunately the first online CRC calculator I encountered delivered pay dirt;  
https://crccalc.com/  
After laboriously entering the first 22 bytes (as hex values), then pressing "Calc CRC-  
16" on the website, the results from 23 CRC-16 algorithms were presented.  
Scanning the list, way down near the bottom an exact match for the last 2 bytes was  
found against CRC-16/MODBUS.  
Trying all the other captured value sets in the table revealed the correct result every  
time.  
CRC-16/MODBUS it is.  
A tidy table-driven approach, using C code, to generate CRC-16/MODBUS checksums  
was found here: http://www.modbustools.com/modbus_crc16.html  
The source code is replicated for convenience on the next page and has been tested  
to work on the tabled data above using Visual Studio 2017.  

<!-- page 18 -->

## Sample Code To Generate CRC-16/MODBUS Checksums

Original source: http://www.modbustools.com/modbus_crc16.html  

(The code on this page uses a symbol font that does not convert. Read the PDF, page 18. The fork implements CRC-16/MODBUS in `components/diesel_heater/heater_protocol.h`.)  

<!-- page 19 -->

## Arduino Sketch To Read Blue Wire
The following source code allows an Arduino that hosts more than one serial port to  
capture data from the blue wire, and relay that into a PC for logging over USB:  
```cpp
/*
Chinese Heater Half Duplex Serial Data Collection Tool
Connects to the blue wire of a Chinese heater, which is the half duplex serial link.
Receives data from serial port 1.
Terminology: Tx is to the heater unit, Rx is from the heater unit.
The binary data is received from the line.
If it has been > 100ms since the last activity this indicates a new frame
sequence is starting, synchronise as such then count off the next 48 bytes
storing them in the Data array.
The "outer loop" then detects the count of 48 and packages the data to be sent
over Serial to the USB attached PC.
Typical data frame timing on the blue wire is:
__Tx_Rx____________________________Tx_Rx____________________________Tx_Rx___________
Rx to next Tx delay is always > 100ms and is paced by the controller.
The delay before seeing Rx data after Tx is usually much less than 10ms.
**The heater only ever sends Rx data in response to a data frame from the controller**
Resultant data is tagged and sent out on serial port 0 (the default debug port),
along with a timestamp for relative timing.
This example works only with boards with more than one serial port like Arduino
Mega, Due, Zero etc.
The circuit:
- Blue wire connected to Serial 1 Rx input - preferably with series 680ohm resistor.
- Serial logging software on Serial port 0 via USB link
created 24 Aug 2018 by Ray Jones
modified 25 Aug by Ray Jones
- simplifed to read 48 bytes, synchronised by observing a long pause
between characters. The heater only sends when prompted.
No longer need to discrimate which packet of data would be present.
This example code is in the public domain.
*/
unsigned long lasttime; // used to calculate inter character delay
void setup()
{
// initialize listening serial port
// 25000 baud, Tx and Rx channels of Chinese heater comms interface:
// Tx/Rx data to/from heater, special baud rate for Chinese heater controllers
Serial1.begin(25000);
// initialise serial monitor on serial port 0
Serial.begin(115200);
// prepare for detecting a long delay
lasttime = millis();
}
void loop()
{
static byte Data[48];
static bool RxActive = false;
static int count = 0;
// read from port 1, the "Tx Data" (to heater), send to the serial monitor:
if (Serial1.available()) {
// calc elapsed time since last rx ' d byte to detect start of frame sequence
unsigned long timenow = millis();
unsigned long diff = timenow - lasttime;
lasttime = timenow;
if(diff > 100) { // this indicates the start of a new frame sequence
RxActive = true;
}
int inByte = Serial1.read(); // read hex byte
if(RxActive) {
Data[count++] = inByte;
if(count == 48) {
RxActive = false;
}
}
}
if(count == 48) { // filled both frames – dump
count = 0;
char str[16];
sprintf(str, "%08d ", lasttime);
Serial.print(str); // print timestamp
for(int i=0; i<48; i++) {
if(i == 0)
Serial.print("Tx "); // insert Tx marker on first pass
if(i == 24)
Serial.print("Rx "); // insert Rx marker after first 24 bytes
sprintf(str, "%02X ", Data[i]); // make 2 dig hex values
Serial.print(str); // and print
}
Serial.println(); // newline and done
} // count == 48
} // loop
```

<!-- page 22 -->

## Sampled Data Runs
The following image is from the first captured data set, using the Arduino sketch.  
The charts are drawn solely from the data frames sent by the heater.  
The data from the controller is not shown. The only data from the controller that  
actually changes whilst running is the sensed temperature and momentary commands  
to start and stop the heater.  
The charts clearly show the operation, especially over the full duration of a run.  
The dramatic drop in fan speed and pump frequency about 80% across are when the  
set temperature was exceeded by 1 °C.  
Shortly thereafter the unit was asked to shutdown.  
You can see the glow pin being reheated for a short while, the pump stopping, then a  
cooldown phase and finally shutdown when the heat exchanger drops below 55 °C.  
One notable exception to the charts is the glow pin current, that rockets up to 10A.  
Also noteworthy is the supply voltage drooping according to the current draw. I may  
need to beef my supply cables up!  
Note that some of the annotations here have since been proven wrong, eg Brd Temp  
and Glow Duty cycle, they are actually fan volatge and glow plug voltage.  

<!-- page 23 -->

## Observed Fault Behaviour
To learn more, certain deliberate errors were placed upon the heater to see how it  
reacted.  
### Wrong Operating Voltage
Using the LCD/LED protected settings menu (passcode = 1688), the system was set to  
24V, with a 12V battery system.  
When set to run, the controller soon reported an E-01 fault, and the LED in the heater  
entered a "flash once" mode. – ala "low voltage" .  
The protocol reported the value 02h in both Rx Byte[3] & Rx Byte[17].  
Interestingly, my Rotary Controller also demands a 24V system, but a no fault is ever  
thrown.  
The distinct difference is Tx Byte[0] is sent as 78h from the Rotary Controller, but 76h  
from the LCD/LED.  
It appears code Tx Byte[0] = 78h causes the operating voltage byte to be ignored (and  
others).  
### Pump Overcurrent
The heater circuit includes a simple current sense on the pump"s current draw, a  
transistor that turns on if the current is too high.  
If the collector of that transistor is deliberately shorted simulating an overcurrent,  
the heater instantly faults out with E-04 "pump failure" .  
Rx Bytes [3] & [17] both report the value 05h.  
### Pump Not Connected
The heater circuit includes monitoring of the voltage after 12V has passed though the  
pump.  
If the pump is not connected, this voltage will never approach 12V when the pump is  
not driven.  
The heater instantly faults out with E-04 "pump failure" when a start is attempted  
without the pump attached.  
Rx Bytes [3] & [17] both report the value 05h.  

<!-- page 24 -->

### Glow Plug Not Connected
If the glow plug is not connected, or open circuit, the heater will attempt to start in  
the usual manner, gently increasing the voltage across the plug until ~9V is reached.  
If no glow plug current is detected when the voltage reaches the maximum, the  
heater faults out with E-03.  
Rx Bytes [3] & [17] both report the value 05h.  
Orange trace is the glow plug current, as sensed by the heater.  
Blue trace is glow plug voltage.  
Pale blue trace is the RunState of the heater (Rx Byte[2])  
Grey trace is the ErrState of the heater (Rx Byte[3])  
Yellow trace indicates elapsed time in seconds divided by 10 (for scaling).  
### Glow Plug Shorted
If the glow plug is shorted, the heater will instantly fault when a start is attempt with  
E-03.  
Rx Bytes [3] & [17] both report the value 05h.  
The glow plug current immediately rises to the 10A limit, and minimal glow plug  
voltage exists:  
Orange trace is the glow plug current, as sensed by the heater.  
Pale blue trace is the RunState of the heater (Rx Byte[2])  
Grey trace is the ErrState of the heater (Rx Byte[3])  
Blue trace is glow plug voltage.  

<!-- page 25 -->

### Thermistor Faults
A shorted thermistor reads as 50 °C.  
An open thermistor reads as -39 °C.  
## Other Observations
### Transition To Run State 5 – Normal Running
When the heater exchanger exceeds 100 °C, the heater seems to consistently switch  
to Run State 5 (Rx Byte[2]).  
This matches the value reported by the heater in Rx Byte[20]…  
### Transition To Shutdown – Run State 0
The heater consistently shuts down when the heater exchanger has cooled below 55 °C.  
### Glow Plug
It appears the heater primarily controls the current going to the glow plug, at 10amps.  
When cold, the current rises to 10A quite quickly, but the voltage slow climbs to ~9V.  
This generally agrees with the resistance increasing with temperature, needing more  
voltage to get the same current.  
The glow plug is driven by a buck regulator in the heater, which receives a 40kHz  
PWM signal from the microcontroller.  

<!-- page 26 -->

The following image shows a typical start:  
Orange trace is the glow plug current, as sensed by the heater.  
Blue trace is glow plug voltage, as sensed by the heater.  
Grey trace is the heat exchanger"s body temperature, as sensed by the heater.  
### System Voltage
The heater PCB can safely operate with a 24V or 12V battery system, just needing an  
LCD or LED controller to define the correct operating voltage to avoid over or under  
voltage errors.  
The 5V for the logic is created via a buck regulator, as is the glow plug.  
The fan is driven by a PWM signal meaning a 24V supply can run a 12V fan given  
appropriate reduction on PWM duty cycle. The required fan speed is regulated using  
feedback from a hall effect sensor driven by magnets embedded in the room air  
intake impeller.  
The pump being a momentary impulse action could possibly run with 24V OK, but I  
note 24V or 12V pumps can be purchased.  
The resistor scaling on the ADC inputs will cope with 32V before saturating.  
Finally, the PCB is labelled 24V against the input power pins.  
### Thermostatic Operation
The LCD Controller can be switched to/from thermostatic mode by holding down the  
"Settings" and "Up" arrow buttons.  
The LCD shows x °C for thermostat mode, "x.x Hz" for pump frequency mode.  

<!-- page 27 -->

The LED seven segment controller likewise can be switched to/from thermostatic  
mode by holding down the "Setting" and "Up" arrow buttons.  
The Rotary Controller can be switched to/from thermostatic mode by holding down  
the ON key for around 8 seconds, it does not matter if the heater is running or not.  
When running, the knob lights red for thermostatic mode, blue for fixed pump  
frequency "Hz mode" .  
When in thermostatic mode, the heater runs at maximum fan and pump speeds until  
the sensed temperature is 1 °C higher than the set value.  
Note that the Rotary Controller only sets the target temperature in 3 °C  
increments Tx Byte[4].  
The heater then backs off to the minimum pump and fan speeds until the  
temperature falls 1 °C below the set temperature.  
This cycle repeats, going from maximum to minimum heater operation, spanning the  
2 °C window.  
### Heater Programming
The LCD or LED seven segment Controllers can change the max/min fan and pump  
speeds.  
i.e. the top end and bottom end fuel mixtures.  
If an LCD or LED Controller is replaced by a Rotary Controller, the heater then  
operates within the limits that were defined by the LCD/LED controller, not the  
values advertised by the Rotary Controller in the data protocol!  
Therefore it appears the heater unit stores these limits in non-volatile EEPROM  
memory within the microcontroller, and these can only be changed by using an  
LCD/LED Controller.  
You do not need to enter the settings menu for this action to take place.  
Likewise, the 12/24V setting is stored.  
My Rotary Controller advertises 24V in the protocol, but does not cause low voltage  
faults.  
However if I set 24V with the LCD, the heater soon faults out with an Undervoltage  
Fault – E-01.  
In terms of the protocol, the only difference appears that Tx Byte[0] is 76h for the  
LCD Controller, and 78h for the Rotary Controller.  
78h appears to inhibit these aspects of the protocol!  

<!-- page 28 -->

### Pump Priming
If Tx Byte[17] is set to the value 5Ah, the pump will run for the purposes for fuel  
priming.  
From the Rotary Controller, priming is initiated by holding the OFF button for a few  
seconds (with the heater off).  
From the LCD/LED Controller, priming is initiated by holding both the "OK/Enter" and  
"Down" buttons until HA-1 appears on the LCD.  
The pump can then be turned on by pressing "Up" , and off by pressing "Down" .  
The relay within the heater should also be heard clicking as it supplies and removes  
power to the pump, fan and glow plug circuits.  
### LCD Heat Bars
The LCD displays a bar graph, roughly showing the temperature of the heat exchanger.  
The threshold levels for each bar to come on are:  
1  
st  
green 2  
nd  
green 1  
st  
yellow 2  
nd  
yellow 1  
st  
red 2  
nd  
red  
21 °C 41 °C 61 °C 81 °C 101 °C 121 °C  
i.e. 20 °C steps, starting from 21 °C  

<!-- page 29 -->

## The "Piccolo"
This entire quest began soon after I first installed my heater.  
Once up to full power or should I say, "full noise" , a horrible resonant whistling was  
coming from within the heater.  
This was very objectional, far worse than the dull low frequency roar immediately  
prior.  
The wife was not happy, nor was I.  
Fiddling about, it was found that crimping the fuel line would cause the heater to  
stop whistling, yet still run OK.  
I sought knowledge from Dr Google and found that LCD Controllers can be used to  
control the air fuel mixture – aha!  
As I only had a Rotary Controller, I bought one off eBay so I could try backing off the  
maximum fuel rate.  
Soon after I had the LCD running the protected menu settings were used to wind back  
the max pump frequency.  
A test run soon confirmed the piccolo was now tamed.  
At some later stage I replaced the Rotary Controller, and it no longer whistled???  
WTF?  
The rest is now history and documented in this document.  
It is the result of me wanting to properly understand how these things go about their  
business.  
I have learnt a LOT, and I wish to share that knowledge back onto the Internet.  
I also would not mind getting an android app working, running the heater via a  
Bluetooth interface.  
Regards,  
Ray Jones  

<!-- page 30 -->

A 5kW heater on the "Autopsy Table" .  
Not dead though, shiny and new, destined for the shed!  
It was an opportunity to test some things like what happens with no fuel, pump  
disconnected/shorted. Glow plug disconnected/shorted etc.  
You can see my Arduino Due patched into the blue wire via a breakout cable I made  
up with some triangular plug/sockets from eBay.  
Data is captured using Putty on a PC, later analysed in Excel:  

