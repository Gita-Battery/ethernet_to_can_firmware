#ifndef TEST_MAIN_H
#define TEST_MAIN_H
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
#define SPI1_SCS_GPIO_Port 0
#define ETH_RST_GPIO_Port 0
#define LED_RX_GPIO_Port 0
#define LED_3_GPIO_Port 0
#define SPI1_SCS_Pin 1
#define ETH_RST_Pin 2
#define LED_RX_Pin 3
#define LED_3_Pin 4
uint32_t HAL_GetTick(void);
void HAL_GPIO_WritePin(unsigned port, unsigned pin, GPIO_PinState state);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t mask);
#endif
