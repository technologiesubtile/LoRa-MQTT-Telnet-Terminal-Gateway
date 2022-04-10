# LoRa-MQTT-Telnet Terminal and Gateway
One unique device that is both LoRa to MQTT gateway and LoRa terminal. Fully configurable and with EEPROM storage of settings.

![bridge_connected](https://user-images.githubusercontent.com/96028811/162629119-c1c4b502-de61-48bd-827e-73210770e9ab.jpg)

The firmware that turns the above LoRa/MQTT interface into a versatile, simultaneous gateway and terminal.

![gateway-terminal_nologo](https://user-images.githubusercontent.com/96028811/162629198-050e7c4e-1648-4000-b565-e1c9c715436e.jpg)

It integrates a number of protocols, including LoRa for the "Remote" side, and serial and a choice among MQTT, telnet server and telnet client on the "Local" side. It is fully user-configurable at runtime and settings are stored in the ESP8266 on-chip EEPROM.
See also the more detailed pdf manual, and the hardware description at http://heinerd.online.fr/elektronik/loraboard1.php3

Portation to the more recent ESP32 based LoRa boards should be straightforward and eliminate the hardware part of the project.
