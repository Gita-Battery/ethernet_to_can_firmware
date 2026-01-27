/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "can.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "wizchip_conf.h"
#include "socket.h"

#include "jsmn.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define True 	1
#define False 	0

#define DEVICE_ID 			0
#define FRIMWARE_VERSION	2
#define HARDWARE_VERSION	2
#define SOCKET_NUMBER		0
#define DATA_BUF_SIZE		2048
#define PORT_UDPS       	56800

#define rxJSON_size 		200
#define txJSON_size 		200


/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint8_t rxJSON[rxJSON_size];
uint8_t txJSON[txJSON_size];

uint8_t destip[4] = {192, 168, 1, 255};

wiz_NetInfo gWIZNETINFO = 	{	.mac	= {0x00, 0x08, 0xdc, 0xab, 0xcd, 0xef},
								.ip		= {192, 168, 1, 101},
								.sn 	= {255, 255, 255, 0},
								.gw 	= {192, 168, 1, 1},
                            };

uint8_t CanMessageReceived;
extern CAN_TxHeaderTypeDef pTxHeader;
extern CAN_RxHeaderTypeDef pRxHeader;
extern uint32_t TxMailbox;
extern uint8_t CanSendArray[8],CanReceiveArray[8];
uint8_t RsRxData[8];
uint8_t RsTxData[8];

uint8_t is_ping = False;
uint8_t connection_is_established = 0;
uint32_t t1 = 0;
uint32_t t2 = 0;

// Add this variable to track LED timing
uint32_t led2_off_timestamp, led3_off_timestamp= 0;
const uint32_t LED_BLINK_DURATION = 20; // Duration of blink in milliseconds

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void W5500_Select(void) {
	HAL_GPIO_WritePin(SPI1_SCS_GPIO_Port, SPI1_SCS_Pin, GPIO_PIN_RESET);
}

void W5500_Unselect(void) {
	HAL_GPIO_WritePin(SPI1_SCS_GPIO_Port, SPI1_SCS_Pin, GPIO_PIN_SET);
}

void W5500_ReadBuff(uint8_t* buff, uint16_t len) {
	HAL_SPI_Receive(&hspi1, buff, len, HAL_MAX_DELAY);
}

void W5500_WriteBuff(uint8_t* buff, uint16_t len) {
	HAL_SPI_Transmit(&hspi1, buff, len, HAL_MAX_DELAY);
}

uint8_t W5500_ReadByte(void) {
	uint8_t byte;
	W5500_ReadBuff(&byte, sizeof(byte));
	return byte;
}

void W5500_WriteByte(uint8_t byte) {
	W5500_WriteBuff(&byte, sizeof(byte));
}

void Ethernet_Init() {
	HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_SET);

	reg_wizchip_cs_cbfunc(W5500_Select, W5500_Unselect);
	reg_wizchip_spi_cbfunc(W5500_ReadByte, W5500_WriteByte);
	reg_wizchip_spiburst_cbfunc(W5500_ReadBuff, W5500_WriteBuff);

	uint8_t rx_tx_buff_sizes[] = {16, 0, 0, 0, 0, 0, 0, 0};
	wizchip_init(rx_tx_buff_sizes, rx_tx_buff_sizes);
	wizchip_setnetinfo(&gWIZNETINFO);
	wizchip_getnetinfo(&gWIZNETINFO);
}

int32_t UDP_Loop(uint8_t sn, uint8_t* buf, uint16_t port)
{
	if (HAL_GetTick() > led2_off_timestamp) {
		HAL_GPIO_WritePin(LED_RX_GPIO_Port, LED_RX_Pin, GPIO_PIN_RESET);
	}

	if (HAL_GetTick() > led3_off_timestamp) {
		HAL_GPIO_WritePin(LED_3_GPIO_Port, LED_3_Pin, GPIO_PIN_RESET);
	}

	memset(&txJSON, '\0', txJSON_size);
	memset(&rxJSON, '\0', rxJSON_size);
	int32_t ret;
	uint16_t size;
	uint16_t destport;

	switch (getSn_SR(sn))
	{
		case SOCK_UDP:
			if (CanMessageReceived)
			{
				connection_is_established = 1;
				t1 = HAL_GetTick();
				pRxHeader.DLC = 8;

				sprintf((char*)txJSON, "{507,232,4,%02X,%lu,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X}",
				pRxHeader.StdId, pRxHeader.DLC, RsTxData[0], RsTxData[1], RsTxData[2], RsTxData[3], RsTxData[4], RsTxData[5], RsTxData[6], RsTxData[7]);

				sendto(sn, txJSON, (strcspn((char*)txJSON, "}") + 1), destip, 56801);
				HAL_GPIO_WritePin(LED_RX_GPIO_Port, LED_RX_Pin, GPIO_PIN_SET);
        		led2_off_timestamp = HAL_GetTick() + LED_BLINK_DURATION;
				CanMessageReceived = 0;
				return 1;
			}
			if ((size = getSn_RX_RSR(sn)) > 0)
			{
				if (size > DATA_BUF_SIZE)
				size = DATA_BUF_SIZE;
				ret = recvfrom(sn, buf, size, destip, (uint16_t*) &destport);
				if (ret <= 0) return ret;
				size = (uint16_t) ret;
				memcpy(&rxJSON, &buf[1], size-2);
				char *token = strtok((char *) rxJSON, ",");
				if(strcmp(token, "007") == 0)
				{
					token = strtok(NULL, ",");
					if(strcmp(token, "232") == 0)
					{
						token = strtok(NULL, ",");
						if(strcmp(token, "001") == 0)
						{
							token = strtok(NULL, ",");
							pTxHeader.StdId = (int)strtol(token, NULL, 16); ;
							token = strtok(NULL, ",");
							pTxHeader.DLC = atoi(token);
							token = strtok(NULL, ",");
							for (int i = 0; i < pTxHeader.DLC; i++)
							{
								sscanf(token + 2*i, "%02x", (unsigned int *) &CanSendArray[i]);
							}
							is_ping = False;
							HAL_CAN_AddTxMessage(&hcan, &pTxHeader, CanSendArray, &TxMailbox);

							HAL_GPIO_WritePin(LED_3_GPIO_Port, LED_3_Pin, GPIO_PIN_SET);
							led3_off_timestamp = HAL_GetTick() + LED_BLINK_DURATION;
//							if(pTxHeader.StdId == 0x200 && CanSendArray[0] == 0x12)
//							{
//								for (int i = 1; i < 32; i++) // Ping first 32 devices
//								{
//									is_ping = True;
//									pTxHeader.StdId = 512 + i;
//									HAL_CAN_AddTxMessage(&hcan, &pTxHeader, CanSendArray, &TxMailbox);
//									HAL_Delay(1);
//								}
//								break;
//							}
						}
					}
				}
//				is_ping = False;
//				HAL_CAN_AddTxMessage(&hcan, &pTxHeader, CanSendArray, &TxMailbox);
//				HAL_GPIO_TogglePin(LED_3_GPIO_Port, LED_3_Pin);
			}
			break;
		case SOCK_CLOSED:
			if((ret = socket(sn, Sn_MR_UDP, PORT_UDPS, 0x01)) != sn) return ret;
			break;
		default:
			break;
	}
	return 1;
}

int32_t TCP_Loop(uint8_t sn, uint8_t* buf, uint16_t port)
{
	memset(&txJSON, '\0', txJSON_size);
	memset(&rxJSON, '\0', rxJSON_size);
	int32_t ret;
	uint16_t size = 0;

	switch(getSn_SR(sn))
	{
		case SOCK_ESTABLISHED :
			if(getSn_IR(sn) & Sn_IR_CON)
			{
				setSn_IR(sn, Sn_IR_CON);
			}
			if (CanMessageReceived)
			{
				t1 = HAL_GetTick();

				txJSON[0] = 0xAA;
				txJSON[1] = pRxHeader.StdId;
				txJSON[2] = pRxHeader.DLC;

				for(int i = 0; i < pRxHeader.DLC; i++)
				{
					txJSON[3 + i] = RsTxData[i];
				}
//				sprintf((char*)txJSON, "{507,232,4,%02lu,%lu,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X}",
//				pRxHeader.StdId, pRxHeader.DLC, RsTxData[0], RsTxData[1], RsTxData[2], RsTxData[3], RsTxData[4], RsTxData[5], RsTxData[6], RsTxData[7]);
				send(sn, txJSON, 3 + pRxHeader.DLC);
				CanMessageReceived = 0;
				break;
			}
			if((size = getSn_RX_RSR(sn)) > 0) // Don't need to check SOCKERR_BUSY because it doesn't not occur.
			{
				if (size > DATA_BUF_SIZE)
				size = DATA_BUF_SIZE;
				ret = recv(sn, buf, size);
				if (ret <= 0) return ret;
				size = (uint16_t) ret;
				memcpy(&rxJSON, &buf[0], size);
				if(rxJSON[0] == 0xFF)
				{
					pTxHeader.StdId = rxJSON[1];
					pTxHeader.DLC	= rxJSON[2];
					for(int i = 0; i < pTxHeader.DLC; i++)
					{
						CanSendArray[i] = rxJSON[3 + i];
					}
				}
				HAL_CAN_AddTxMessage(&hcan, &pTxHeader, CanSendArray, &TxMailbox);
			}
			break;
		case SOCK_CLOSE_WAIT :
			if((ret = disconnect(sn)) != SOCK_OK) return ret;
			break;
		case SOCK_INIT :
			if( (ret = listen(sn)) != SOCK_OK) return ret;
			break;
		case SOCK_CLOSED:
			if((ret = socket(sn, Sn_MR_TCP, port, 0x00)) != sn) return ret;
			break;
		default:
			break;
   }
   return 1;
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_CAN_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in freertos.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{

	HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &pRxHeader, CanReceiveArray);
	memcpy(RsTxData, CanReceiveArray, 8);
	CanMessageReceived = 1;
}
/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
