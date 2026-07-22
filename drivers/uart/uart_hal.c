#include "uart/uart_hal.h"
#include "uart/gpio.h"

/**
 * @file uart_hal.c
 * @brief UART Hardware Abstraction Layer for ESP32-C3.
 *
 * This implementation handles clock gating, baud rate calculation, 
 * and FIFO management for UART0/1 without ESP-IDF dependencies.
 */

/**
 * @brief Returns the frequency of the selected source clock.
 */

uint32_t get_clk_freq(uart_sclk_t sclk) {
    switch (sclk) {
        case UART_SCLK_APB:  return 40000000;
        case UART_SCLK_XTAL: return 40000000;
        case UART_SCLK_RTC:  return 17500000; // Valore indicativo
        default:             return 80000000;
    }
}

/**
 * @brief Calculates the baud rate divisor (Integral + Fractional).
 * Formula: div = (Freq << 4) / Baud
 */
divisor_t get_clk_div(uint32_t freq, uint32_t baudrate){ 
    uint32_t div_val = (uint32_t)((((uint64_t)freq << 4) + (baudrate / 2)) / baudrate);
    divisor_t divisor;
    divisor.integral = div_val >> 4;
    divisor.frag = div_val & 0xF;
    return divisor;
}

/**
 * @brief Initializes the UART peripheral with the specified configuration.
 *
 * This function performs the full hardware handshake:
 * 1. Enables peripheral clock gating and performs a hardware reset.
 * 2. Configures the clock source and baud rate generator.
 * 3. Sets the frame format (data bits, stop bits, parity).
 * 4. Flushes the hardware FIFOs and synchronizes configuration registers.
 *
 * @param config Pointer to a constant uart_config_t structure.
 * @pre GPIOs must be configured via hal_gpio_uart_setup() for external communication.
 */
void hal_uart_init(uart_config_t *config){
    
    /*--- 1. INITIALISATION ---*/
    
    // 1.1 UART global clock enabling
    SYSTEM_PERIP_CLK_EN0_REG |= BIT(SYSTEM_UART_MEM_CLK_EN);
    
    // 1.2 Clock enable and reset
    uint32_t port_bit = (config->port == 1) ? BIT(SYSTEM_UART1_CLK_EN_RST) : BIT(SYSTEM_UART0_CLK_EN_RST);
    
    SYSTEM_PERIP_CLK_EN0_REG |= port_bit;
    SYSTEM_PERIP_CLK_EN0_REG &= ~port_bit;
    
    UART_REG.clk_conf |= BIT(UART_RST_CORE);

    SYSTEM_PERIP_RST_EN0_REG |= port_bit;
    SYSTEM_PERIP_RST_EN0_REG &= ~port_bit;
    
    // 1.3 Disable reset clock and uart synch configuration 
    UART_REG.clk_conf &= ~BIT(UART_RST_CORE);
    UART_REG.uart_id &= ~BIT(UART_REG_CTRL);

    
    /*--- 2. CONFIGURATION ---*/
    
    // 2.1 Waiting for synch
    while (UART_REG.uart_id & BIT(UART_REG_UPDATE)) {}
    
    // 2.2 UART clock configuration
    UART_REG.clk_conf = (UART_REG.clk_conf & ~(0x3 << UART_SCLK_SEL))|((config->src_clock & 0x3) << UART_SCLK_SEL); 
    UART_REG.clk_conf |= BIT(UART_SCLK_EN);

    uint32_t freq = get_clk_freq(config->src_clock);
    
    divisor_t divisor = get_clk_div(freq, config->baud_rate);
    
    UART_REG.clkdiv |= ((divisor.integral & 0xFFF) | ((divisor.frag & 0xF) << UART_CLKDIV_FRAG));
    UART_REG.conf0 |= (config->data << UART_BIT_NUM)|(config->stop << UART_STOP_BIT_NUM);
    UART_REG.conf0 |= (config->parity_check << UART_PARITY)|BIT(UART_PARITY_EN);
    
    /*--- 3. TX/RX ENABLING ---*/

    UART_REG.conf0 |= BIT(UART_RXFIFO_RST) | BIT(UART_TXFIFO_RST);
    UART_REG.conf0 &= ~(BIT(UART_RXFIFO_RST) | BIT(UART_TXFIFO_RST));
    
    UART_REG.uart_id |= BIT(UART_REG_UPDATE);

    UART_REG.conf1 |= ((10 & 0x7F) << UART_RXFIFO_FULL_THRHD);
    
    UART_REG.uart_id &= ~BIT(UART_REG_CTRL);
    UART_REG.uart_id |= BIT(UART_REG_UPDATE);

    while (UART_REG.uart_id & BIT(UART_REG_UPDATE)) {}

    return;    
}


/**
 * @brief Configures the IO_MUX for UART communication.
 *
 * Sets up the internal signal routing:
 * - Configures IO_MUX for the correct function mode and enables input buffers.
 *
 * @note This configuration is specific to the ESP32-C3 pinout.
 */
void hal_gpio_uart_setup() {
     
    // GPIO config in IO_MUX
    IO_MUX.gpio_21 |= BIT(IO_MUX_GPIO21_MCU_SEL); // Function 1
    IO_MUX.gpio_20 |= BIT(IO_MUX_GPIO20_MCU_SEL) | BIT(IO_MUX_GPIO20_FUN_IE); // Function 1 + Input Enable
}


/**
 * @brief Transmits a single byte through the UART FIFO.
 *
 * This is a blocking function. It will wait until there is enough
 * space in the hardware TX FIFO before writing the byte.
 *
 * @param byte The 8-bit data to be transmitted.
 */
void hal_uart_write_byte(uint8_t byte){
    
    while (((UART_REG.status >> 16) & 0xFF) >= 128) {}

    UART_REG.fifo = byte;    
}


/**
 * @brief Attempts to read a single byte from the UART RX FIFO.
 *
 * This is a non-blocking function. It checks if any data is available
 * in the RX FIFO before attempting to read.
 *
 * @param[out] data Pointer to the buffer where the received byte will be stored.
 * @return uint8_t Returns 1 if a byte was successfully read, 0 if the FIFO is empty.
 */
uint8_t hal_uart_read_byte(uint8_t *data){
    
    uint8_t rx_count = (UART_REG.status & 0xFF);

    if(rx_count > 0){
        *data = (uint8_t)(UART_REG.fifo & 0xFF);
        return 1;
    }
    return 0;
}

/* void hal_uart_write_string(uint8_t *byte_string){
    uint8_t *curr_byte = byte_string;
    while((((UART_REG.status >> 16) & 0xFF) <= 128)||curr_byte != NULL){
        UART_REG.fifo = *curr_byte;
        curr_byte = byte_string + sizeof(uint8_t);
    }
}*/


