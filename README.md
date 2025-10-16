# MOG-CHAT V1 (RELEASE)
---
## Features:
- Browser accessible chat
- AP
- RealTime
- Modern interface
- Runs fully on the ESP32WROOM32U

## How to get it to work?
- You need to have ArduinoIDE installed

*Do this if you still haven't:*
- Then you have to go to **File → Preferences**,  in the **Additional Boards Manager URLs** field, add this line: ```https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json```
- Then go to **Tools → Board → Boards Manager**, search for `esp32`, and click **Install**.
- Then go to **Tools → Board → ESP32 Arduino → ESP32 Dev Module**. (Select the ESP32 DEV MODULE)
- Settings:
 > Upload speed: 115200
 > Flash Frequency: 80MHz
 > Flash Mode: QIO
- Under **Tools → Port**, select the COM port where your ESP32 is connected, and select the "ESP32 DEV MODULE" if you still haven't.

- Download the MOG-CHAT ino file. Open it in ArduinoIDE. (there will be a bin file later too.)
- And upload it.

## How to use
- Plug the ESP32WROOM32U via pc or anywhere else.
- Go to your WIFI selection in your phone, pc or any device.
- Find CHAT or whatever you named it. And the current password for it is "mogchat123".
- Open your browser and paste ```http://192.168.4.1```

# This readme will change every release
