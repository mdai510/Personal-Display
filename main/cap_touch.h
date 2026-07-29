#include "i2c.h"

#define PCA9557_I2C_ADDRESS 0x18 //from datasheet A0-A2 tied to ground
#define GT911_I2C_ADDRESS 0x5D

#define PCA9557_REG_INPUT 0x00
#define PCA9557_REG_OUTPUT 0x01
#define PCA9557_REG_POLARITY 0x02
#define PCA9557_REG_CONFIG 0x03

#define PCA9557_IO0 (1U << 0)
#define PCA9557_IO1 (1U << 1)

#define GT911_REG_STATUS 0x814E
#define GT911_PT1_X_COORD_LOW_BYTE_REG 0x8150
#define GT911_PT1_X_COORD_HIGH_BYTE_REG 0x8151
#define GT911_PT1_Y_COORD_LOW_BYTE_REG 0x8152
#define GT911_PT1_Y_COORD_HIGH_BYTE_REG 0x8153

esp_err_t cap_touch_init(void);
esp_err_t cap_touch_read(uint16_t *x, uint16_t *y, bool *touch_detected);
