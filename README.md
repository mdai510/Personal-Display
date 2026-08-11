Personal Weather Display

Features:  
User can change location/Wi-Fi config through Bluetooth LE connection (or manual setting if wanted)  
Non-Volatile Storage saves location and Wi-Fi config  
Main display shows current weather, 24-hour forecast, and 7-day forecast  
Swipe/Touch buttons to change display  
Manual refresh button to call weather API  
Auto-refresh of weather data every 30 mins  
Wi-Fi station and BLE GAP Advertising / GATT server only on during operation (API calls or Wi-Fi/location config)  
SNTP Time Synchronization  
  
Components:  
CrowPanel ESP32 HMI 7.0-inch Display (ESP32-S3)  
GT911 Capacitive Touch  
PCA9557 I/O Expander  
NimBLE  
ESP-IDF  

<img width="2994" height="1544" alt="image" src="https://github.com/user-attachments/assets/12a181a6-0f88-45d6-a889-9d8ee1c2fcbe" />  

Display Demo Video (UI + touchscreen): 
https://github.com/user-attachments/assets/3fbd07ed-7a2e-4cae-93bb-e132db5bea91  

Bluetooth Connect Demo Video:  
Video shows Wi-Fi failing at first, then a bad and good BLE Wi-Fi request  
Then, it demonstrates changing location  
https://github.com/user-attachments/assets/7ab603d7-eb20-4904-991c-e32ce47bcc0e  
(currently just using nRF app on Android, if I have time to create an app for this I will :)  

Settings/Setup:  
1. Install ESP-IDF v6.0.1  
2. Install ESP-IDF VSCode Extension
3. Clone the project folder  
4. Open the folder in VS Code.  
5. Select the installed ESP-IDF version (ctrl+shift+p -> ESP-IDF: Select Current ESP-IDF Version).  
6. Build, select the serial port, then flash.   
     
If you want to skip bluetooth wifi/location configuration:  
set your wifi ssid and password in ESP-IDF configuration editor or by changing the values in sdkconfig.h  
in main.c set location info
(i included my sdkconfig file with personal info removed, since many BLE config values, LCD pin values, and buffer size values were changed)  

Sources:  
https://www.elecrow.com/wiki/esp32-display-702727-intelligent-touch-screen-wi-fi26ble-800480-hmi-display.html  
https://erikflowers.github.io/weather-icons/  
https://docs.espressif.com (NVS, BLE, I2C, WiFi Event Loop  
https://open-meteo.com/en/docs  
https://ip-api.com  
https://learn.adafruit.com/introduction-to-bluetooth-low-energy  
[pca9557.pdf](https://github.com/user-attachments/files/30683521/pca9557.pdf)  
[GT911_registers.pdf](https://github.com/user-attachments/files/30683518/GT911_registers.pdf)  
[GT911.pdf](https://github.com/user-attachments/files/30683515/GT911.pdf)  

Future Work:
Make Bluetooth app  
Add additional modules (but space is a concern so maybe not)  

