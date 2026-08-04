Personal Display

Features:  
Relative location from IP-API (there is a setting to set coordinates manually, since ip location is very approximate; this is what i have done)
Weather information from Open-Meteo  
24-forecast  
7-day forecast  
Swipe/Touch buttons to change display  
  
Components:  
CrowPanel ESP32 HMI 7.0-inch Display  
GT911 Capacitive Touch  
PCA9557 I/O Expander  
ESP-IDF

<img width="3120" height="1780" alt="image" src="https://github.com/user-attachments/assets/c58ea5af-f8f4-422f-a9ea-3d733a4589ab" />
<img width="3214" height="1828" alt="image" src="https://github.com/user-attachments/assets/91cde4c9-111f-40d7-af85-a01e10f3410e" />

Settings:
set your wifi ssid and password in ESP-IDF configuration editor
in main.c if you want more accurate location, set lat/lon and timezone
#define CONFIG_DISPLAY_USE_SINGLE_FB 1
#define CONFIG_DISPLAY_LCD_DATA_LINES_16 1
#define CONFIG_DISPLAY_LCD_DATA_LINES 16
#define CONFIG_DISPLAY_LCD_VSYNC_GPIO 40
#define CONFIG_DISPLAY_LCD_HSYNC_GPIO 39
#define CONFIG_DISPLAY_LCD_DE_GPIO 41
#define CONFIG_DISPLAY_LCD_PCLK_GPIO 0
#define CONFIG_DISPLAY_LCD_DATA0_GPIO 15
#define CONFIG_DISPLAY_LCD_DATA1_GPIO 7
#define CONFIG_DISPLAY_LCD_DATA2_GPIO 6
#define CONFIG_DISPLAY_LCD_DATA3_GPIO 5
#define CONFIG_DISPLAY_LCD_DATA4_GPIO 4
#define CONFIG_DISPLAY_LCD_DATA5_GPIO 9
#define CONFIG_DISPLAY_LCD_DATA6_GPIO 46
#define CONFIG_DISPLAY_LCD_DATA7_GPIO 3
#define CONFIG_DISPLAY_LCD_DATA8_GPIO 8
#define CONFIG_DISPLAY_LCD_DATA9_GPIO 16
#define CONFIG_DISPLAY_LCD_DATA10_GPIO 1
#define CONFIG_DISPLAY_LCD_DATA11_GPIO 14
#define CONFIG_DISPLAY_LCD_DATA12_GPIO 21
#define CONFIG_DISPLAY_LCD_DATA13_GPIO 47
#define CONFIG_DISPLAY_LCD_DATA14_GPIO 48
#define CONFIG_DISPLAY_LCD_DATA15_GPIO 45
