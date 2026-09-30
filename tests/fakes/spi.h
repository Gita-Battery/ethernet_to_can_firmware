#ifndef TEST_SPI_H
#define TEST_SPI_H
#include "main.h"
typedef struct { unsigned unused; } SPI_HandleTypeDef;
extern SPI_HandleTypeDef hspi1;
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *handle);
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *handle);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *handle, uint8_t *buffer, uint16_t length, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *handle, uint8_t *buffer, uint16_t length, uint32_t timeout);
#endif
