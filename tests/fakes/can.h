#ifndef TEST_CAN_H
#define TEST_CAN_H
#include "main.h"
#define CAN_RX_FIFO0 0
#define CAN_ID_STD 0
#define CAN_RTR_DATA 0
#define CAN_ESR_EWGF 1
#define CAN_ESR_EPVF 2
#define CAN_ESR_BOFF 4
#define CAN_ESR_LEC 0x70
#define CAN_TX_MAILBOX0 1
#define CAN_TX_MAILBOX1 2
#define CAN_TX_MAILBOX2 4
typedef struct { uint32_t ESR; } CAN_TypeDef;
typedef struct { CAN_TypeDef *Instance; } CAN_HandleTypeDef;
typedef struct { uint32_t StdId, ExtId, IDE, RTR, DLC, TransmitGlobalTime; } CAN_TxHeaderTypeDef;
typedef struct { uint32_t StdId, ExtId, IDE, RTR, DLC; } CAN_RxHeaderTypeDef;
extern CAN_HandleTypeDef hcan;
uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *handle, uint32_t fifo);
HAL_StatusTypeDef HAL_CAN_GetRxMessage(CAN_HandleTypeDef *handle, uint32_t fifo,
                                      CAN_RxHeaderTypeDef *header, uint8_t *data);
uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *handle);
HAL_StatusTypeDef HAL_CAN_AddTxMessage(CAN_HandleTypeDef *handle,
                                     CAN_TxHeaderTypeDef *header, uint8_t *data, uint32_t *mailbox);
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *handle);
HAL_StatusTypeDef HAL_CAN_AbortTxRequest(CAN_HandleTypeDef *handle, uint32_t mailboxes);
#endif
