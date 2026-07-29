#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include <stdbool.h>

#define I2C_MASTER_PORT I2C_NUM_0

#define SDA_IO_NUM GPIO_NUM_19
#define SCL_IO_NUM GPIO_NUM_20

esp_err_t i2c_start_master(void);
bool i2c_has_master_started(void);
esp_err_t i2c_get_master_handle(i2c_master_bus_handle_t *out_handle);
esp_err_t i2c_stop_master(void);
esp_err_t i2c_device_create(i2c_master_dev_handle_t *dev_handle, uint16_t device_address);