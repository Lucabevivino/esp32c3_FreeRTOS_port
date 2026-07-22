#include "FreeRTOS.h"
#include "task.h"
#include "uart/uart_hal.h"

void uart_task(void *arg)
{
    const char* hello_world = "Hello world!\n";
    uart_config_t *cfg = (uart_config_t *)arg;

    hal_gpio_uart_setup();
    hal_uart_init(cfg);

    const char* p = hello_world;
    while (*p != '\0') { hal_uart_write_byte(*p++); }

    vTaskDelete(NULL);
}

int main(void)
{
    static uart_config_t cfg;
    cfg.src_clock = UART_SCLK_APB;
    cfg.baud_rate = 115200;
    cfg.parity_check = 0;
    cfg.port = UART_0;
    cfg.data = 0;
    cfg.stop = 0;
   
    xTaskCreate(
        uart_task,     
        "uart_task",   
        &cfg,         
        1,            
        NULL          
    );

    vTaskStartScheduler();

    while (1) {}
}
