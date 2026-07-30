#include "cap_touch.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "cap_touch";
static TaskHandle_t s_cap_touch_task_handle = NULL;

static i2c_master_dev_handle_t gt911_handle;
static i2c_master_dev_handle_t pca9557_handle;

static uint8_t pca9557_output = 0x00; // Keep track of the output state of PCA9557 pins
static uint8_t pca9557_config = 0xFF; // Keep track of the configuration state of PCA9557 pins (1=input, 0=output)

/*
* Here, 2 PCA9557 GPIO outputs are connected to GT911 touch controller's reset and interrupt pins.
* We can control the GT911's reset and interrupt pins via I2C commands to the PCA9557.
*/

static esp_err_t pca9557_write(uint8_t reg_addr, uint8_t value) {
    if (pca9557_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[2] = {reg_addr, value};
    return i2c_master_transmit(pca9557_handle, data, 
        sizeof(data), 100);
}

/*
* Since reset pin on PCA9557 is tied high, 
* we can reset by manually writing registers to known powerup values (just in case)
*/
static esp_err_t pca9557_reset(void){
    //set all PCA9557 pins to input mode, output low, and pins to default polarity
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_CONFIG, pca9557_config), "cap_touch",
                      "Failed to set PCA9557 config register");
    pca9557_output = 0x00;
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_OUTPUT, pca9557_output), "cap_touch",
                      "Failed to set PCA9557 output register");
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_POLARITY, 0xF0), "cap_touch",
                      "Failed to set PCA9557 polarity register");
    return ESP_OK;
}

static esp_err_t cap_touch_reset(void){
    ESP_RETURN_ON_ERROR(pca9557_reset(), "cap_touch", "Failed to reset PCA9557");
    //set PCA9557 IO0 and IO1 pins to output mode
    pca9557_config = 0xFC;
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_CONFIG, pca9557_config), "cap_touch",
                      "Failed to set PCA9557 config register for output mode");

    //now reset GT911 by toggling PCA9557 IO0/IO1 pins, 
    //which are connected to GT911's reset and interrupt pins
    //reset sequence to set GT911 address to 0x5D:
    //RESET and INT low (already low, but just set anyways)
    pca9557_output &= ~(PCA9557_IO0 | PCA9557_IO1);
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_OUTPUT, pca9557_output), "cap_touch",
                      "Failed to set PCA9557 output register for reset");
    //hold for at least 10ms
    vTaskDelay(pdMS_TO_TICKS(20));
    //release reset
    pca9557_output |= PCA9557_IO0;
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_OUTPUT, pca9557_output), "cap_touch",
                      "Failed to set PCA9557 output register for reset release");
    //hold for at least 50ms
    vTaskDelay(pdMS_TO_TICKS(100));
    //change interrupt pin to input mode so GT911 can drive it
    pca9557_config |= PCA9557_IO1;
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_CONFIG, pca9557_config), "cap_touch",
                      "Failed to set PCA9557 config register for interrupt pin");
    return ESP_OK;
}

void cap_touch_task(void *pvParameters){
  while (1) {
    static uint16_t x, y;
    static uint16_t last_x, last_y = UINT16_MAX;
    static bool touch_detected;
    esp_err_t ret = cap_touch_read(&x, &y, &touch_detected);
    if (ret == ESP_OK && touch_detected) {
        if(x != last_x || y != last_y){
          ESP_LOGI(TAG, "Touch detected at coordinates: (%u, %u)", x, y);
          // Handle touch event here
          last_x = x;
          last_y = y;
        }
        //here, touch coord same as last, so no new touch
    } else if (ret == ESP_ERR_NOT_FOUND) {
      // No new touch data available
    } else if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Error reading capacitive touch: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // Polling delay
  }
}

esp_err_t cap_touch_init(void) {
    if(!i2c_has_master_started()) {
        if(i2c_start_master() != ESP_OK) {
            return ESP_FAIL;
        }
    }

    i2c_master_bus_handle_t master_handle;
    if(i2c_get_master_handle(&master_handle) != ESP_OK) {
        return ESP_FAIL;
    }
    if(i2c_device_create(&gt911_handle, GT911_I2C_ADDRESS) != ESP_OK) {
        return ESP_FAIL;
    }
    if(i2c_device_create(&pca9557_handle, PCA9557_I2C_ADDRESS) != ESP_OK) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(cap_touch_reset(), "cap_touch", "Failed to reset cap touch");

    //register cap touch task
    xTaskCreate(cap_touch_task, "cap_touch_task", (4 * 1024), NULL, 1, &s_cap_touch_task_handle);

    return ESP_OK;
}

static esp_err_t gt911_read(uint16_t reg_addr, uint8_t *reg_value, size_t value_len) {
    if(gt911_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if(reg_value == NULL || value_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addr_bytes[2] = {(uint8_t)(reg_addr >> 8), (uint8_t)(reg_addr & 0xFF)};

    esp_err_t ret = i2c_master_transmit_receive(gt911_handle, addr_bytes, 
        sizeof(addr_bytes), reg_value, value_len, 100);
    return ret;
}

static esp_err_t gt911_write_u8(uint16_t reg_addr, uint8_t write_value) {
    if(gt911_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    //alloc buffer for 16 bit address + value
    uint8_t data[3] = {(uint8_t)(reg_addr >> 8), (uint8_t)(reg_addr & 0xFF), write_value};

    esp_err_t ret = i2c_master_transmit(gt911_handle, data, 
        sizeof(data), 100);
    return ret;
}

/*
* Check data valid bit to see if GT911 has new touch data available.
* If so read the touch coordinates from GT911 and return them.
*/
esp_err_t cap_touch_read(uint16_t *x, uint16_t *y, bool *touch_detected) {
    if(x == NULL || y == NULL || touch_detected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status;
    ESP_RETURN_ON_ERROR(gt911_read(GT911_REG_STATUS, &status, 
        sizeof(status)), "cap_touch", "Failed to read GT911 status register");
    if((status & 0x80) == 0) { //data valid bit is not set, no new touch data
      /*
      * This doesn't mean that there is no touch detected, 
      * it just means that the GT911 has not updated its touch data since the last read.
      * For functions using this function, they should check the returned esp_err_t value
      * and check for ESP_ERR_NOT_FOUND to determine if there is no new touch data available.
      */
      *touch_detected = false; 
      return ESP_ERR_NOT_FOUND;
    }
    if((status & 0x0F) == 0) {
        //no touch points detected
        *touch_detected = false;
        status = 0;
        ESP_RETURN_ON_ERROR(gt911_write_u8(GT911_REG_STATUS, status), "cap_touch", "Failed to clear GT911 status register");
        return ESP_OK;
    }
    //read touch coordinates from GT911
    uint8_t coords[4];
    ESP_RETURN_ON_ERROR(gt911_read(GT911_PT1_X_COORD_LOW_BYTE_REG, coords, sizeof(coords)), "cap_touch", "Failed to read GT911 touch coordinates");

    *x = ((uint16_t)coords[1] << 8) | coords[0];
    *y = ((uint16_t)coords[3] << 8) | coords[2];
    *touch_detected = true;

    //clear the status register to indicate that we have read the touch data
    status = 0;
    ESP_RETURN_ON_ERROR(gt911_write_u8(GT911_REG_STATUS, status), "cap_touch", "Failed to clear GT911 status register");
    return ESP_OK;
}
