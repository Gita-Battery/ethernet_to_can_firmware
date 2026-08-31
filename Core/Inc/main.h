/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
void	Ethernet_Init(void);

int32_t UDP_Loop(uint8_t sn, uint8_t* buf, uint16_t port);
int32_t TCP_Loop(uint8_t sn, uint8_t* buf, uint16_t port);

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define SPI1_SCS_Pin GPIO_PIN_0
#define SPI1_SCS_GPIO_Port GPIOB
#define ETH_RST_Pin GPIO_PIN_1
#define ETH_RST_GPIO_Port GPIOB
#define ETH_INT_Pin GPIO_PIN_2
#define ETH_INT_GPIO_Port GPIOB
#define ETH_INT_EXTI_IRQn EXTI2_IRQn
#define LED_RX_Pin GPIO_PIN_12
#define LED_RX_GPIO_Port GPIOB
#define LED_TX_Pin GPIO_PIN_13
#define LED_TX_GPIO_Port GPIOB
#define LED_3_Pin GPIO_PIN_14
#define LED_3_GPIO_Port GPIOB
#define LED_4_Pin GPIO_PIN_15
#define LED_4_GPIO_Port GPIOB
#define RS485_EN_Pin GPIO_PIN_8
#define RS485_EN_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
