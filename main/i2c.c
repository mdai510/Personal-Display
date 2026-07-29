#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include "i2c.h"
#include <stdbool.h>

static i2c_master_bus_handle_t master_bus_handle;

esp_err_t i2c_start_master(void){
    i2c_master_bus_config_t master_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = SDA_IO_NUM,
        .scl_io_num = SCL_IO_NUM,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&master_bus_config, &master_bus_handle);
}

bool i2c_has_master_started(void){
    return master_bus_handle != NULL;
}

esp_err_t i2c_get_master_handle(i2c_master_bus_handle_t *out_handle){
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!i2c_has_master_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_handle = master_bus_handle;
    return ESP_OK;
}

esp_err_t i2c_stop_master(void){
    return i2c_del_master_bus(master_bus_handle);
}

esp_err_t i2c_device_create(i2c_master_dev_handle_t *dev_handle, uint16_t device_address){
    i2c_device_config_t device_config = {
        .device_address = device_address,
        .dev_addr_length = I2C_ADDR_BIT_7,
        .scl_speed_hz = 100000,
    };

    return i2c_master_bus_add_device(master_bus_handle, &device_config, dev_handle);
}