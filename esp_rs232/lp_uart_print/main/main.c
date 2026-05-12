#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#define UART_PORT_NUM      UART_NUM_0  // Use UART0 (connected to USB)
#define UART_BAUD_RATE     115200
#define BUF_SIZE           1024

static const char *TAG = "UART_Standard";

void app_main(void)
{
    // Configure UART parameters
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Install driver and apply configuration
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));

    // Send "Hello" and the Date
    const char* msg = "\r\nHello! Today is April 29, 2026.\r\nESP32 Ready for Echo...\r\n";
    uart_write_bytes(UART_PORT_NUM, msg, strlen(msg));

    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(UART_PORT_NUM, data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        
        if (len > 0) {
            data[len] = '\0';
            
            // Echo logic: Send back what was received
            uart_write_bytes(UART_PORT_NUM, "Echo: ", 6);
            uart_write_bytes(UART_PORT_NUM, (const char *)data, len);
            uart_write_bytes(UART_PORT_NUM, "\r\n", 2);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}