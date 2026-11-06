#ifndef GPIO_DEFS_H
#define GPIO_DEFS_H

// Interfaz I2C1 //
#define I2C1_SCL_PIN 1
#define I2C1_SDA_PIN 2

// Interfaz I2C2 //
#define I2C2_SCL_PIN 17
#define I2C2_SDA_PIN 18

// Interfaz SPI //
#define SPI_FLASH_CS 9
#define SPI_DSP_CS 14
#define SPI_MOSI 11
#define SPI_MISO 10
#define SPI_CLK 12

// Interfaz SPI_E (expansion) //
#define SPIE_CS 4
#define SPIE_MOSI 6
#define SPIE_MISO 5
#define SPIE_CLK 7

// Interfaz UART //
#define UART_TX 16
#define UART_RX 15

// Pines de interrupcion //
#define TECLADO_INT_PIN 39
#define FUELG_INT_PIN 41
#define CHARGER_INT_PIN 42

// Entradas de datos Wiegand //
#define WD0_PIN 38
#define WD1_PIN 40

// Entradas de pulsos //
#define PULSE1_IN_PIN 19
#define PULSE2_IN_PIN 20

// Control de brillo del display //
#define DSP_LED 13



#endif // GPIO_DEFS_H
