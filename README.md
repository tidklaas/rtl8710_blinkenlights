# rtl8710_blinkenlights
## "More light!" -- J.W. v. Goethe, last words

```
ACHTUNG!
ALLES TURISTEN UND NONTEKNISCHEN LOOKENPEEPERS!

DAS KOMPUTERMASCHINE IST NICHT FÜR DER GEFINGERPOKEN 
UND MITTENGRABEN! ODERWISE IST EASY TO SCHNAPPEN DER
SPRINGENWERK, BLOWENFUSEN UND POPPENCORKEN MIT
SPITZENSPARKEN.

IST NICHT FÜR GEWERKEN BEI DUMMKOPFEN. DER RUBBERNECKEN
SIGHTSEEREN KEEPEN DAS COTTONPICKEN HÄNDER IN DAS POCKETS
MUSS.

ZO RELAXEN UND WATSCHEN DER BLINKENLICHTEN.
```
### About
This is a small test project for Realtek's RTL8710 "Ameba" SoC. As with
any new microcontroller one encounters, the first thing to do is to make
it blink a LED. But this is the future, we can do better. We can blink
hundreds of LEDs!

This project allows you to control a WS2812B RGB LED strip via WiFi by
providing an AP and a web server.

### Prerequisites
You will need a RTL87810 module, an ARM Cortex-M compatible SWD debugger,
a simple electronic circuit for shifting the SoC's 3.3V logic level to
the strip's 5V and, of course, a WS2812B LED strip.

RTL8710 modules are easy to find on eBay or Aliexpress, the B&T BT-00 variant
is hard to miss. Pine64 is about to release their own version of this module,
called PADI: https://www.pine64.org/?page_id=946

If you are looking for a development board with integrated SWD debugger, I can
recommend the Ameba RTL8710 Board http://www.amebaiot.com/en/ameba-sdk-boards/
Those can be bought for around $20 on Aliexpress.

You will also need a copy of the GCC based SDK for this chip. It can be
downloaded from Pine64's PADI web site. Look under Resources and download
the "Ameba RTL8710AF SDK ver v3.5a GCC ver 1.0.0- without NDA"

After extracting the SDK, follow the instructions found in
"doc/UM0096 Realtek Ameba-1 build environment setup - gcc.pdf" to prepare
the SDK. If you are using OpenOCD, you should also change the JTAG clock speed
from 10kHz to 1MHz in component/soc/realtek/8195a/misc/gcc_utility/openocd/ameba1.cfg

Make sure you can build and upload the standard SDK firmware. If everything
works, make a "clean_all" and clone this repository into the project folder.
This project can be built in the same way as the standard project, just use
the folder rtl8710_blinkenlights/GCC-RELEASE as base folder.

### Blinkenlights
After uploading the new firmware, the module should start a WiFi-AP and
provide a simple web server. On first start it may rely on a previous
WiFi AP configuration being stored in flash, so you might have to use the
default AT command firmware first to set up the AP. In any case, use ramdebug
first to verify that the new firmware is working before overwriting the
default firmware.

If unexpectedly everything works as it should, you can now connect to the
module's WiFi network and access the configuration web server at
http://192.168.1.1
The root page lets you tweak the LED strip configuration.
The WiFi configuration can be changed at http://192.168.1.1/wifi

### Wiring up the Hardware
The WS2812 bit stream will be sent on GPIO pin GC2. If you are only running one
or two WS2812s, you can get away with connecting them directly to the module's
VCC and GND. They will not be very bright but it is an easy way to check that
a valid bit stream is generated. If you are driving more LEDs, you will need a
dedicated 5V power supply and a level shifter for the GPIO pin. Any 74HCT...
buffer should do, I use a 74HCT125N. Connect module, buffer and LED strip to
common ground, module's GC2 to buffer input, strip's data input to buffer
output.

Debugging information will be printed on the Log-UART, which is connected to
GPIOs GB0 (TX) and GB1 (RX). On the Ameba board, these pins can be connected
to the debugger via the select switch and will be routed to the virtual console.
If you are using a BT-00, connect a USB-to-serial adapterto the corresponding
pads. 

Debugging information will be printed on the Log-UART, which is connected to
GPIOs GB0 (TX) and GB1 (RX). On the Ameba board, these pins can be connected
to the debugger via the select switch and will be routed to the virtual console.
If you are using a BT-00, connect a USB-to-serial adapter to the corresponding
pads.

### Misc
More information about and support for the RTL8710 can be found on the 
RTL8710 Community Forum: https://www.rtl8710forum.com/

