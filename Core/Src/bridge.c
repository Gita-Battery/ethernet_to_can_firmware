#include "bridge.h"
#include "bridge_protocol.h"
#include "eth_io.h"
#include "main.h"
#include "can.h"
#include "spi.h"
#include "cmsis_os.h"
#include "socket.h"
#include <string.h>

#define BRIDGE_SOCKET 0U
#define BRIDGE_RX_PORT 56800U
#define BRIDGE_TX_PORT 56801U
#define SPI_TIMEOUT_MS 10U
#define RETRY_MS 1000U

volatile BridgeDiagnostics bridge_diagnostics;
static BridgeFrame can_queue[BRIDGE_CAN_QUEUE_SIZE];
static uint32_t queue_head, queue_tail, queue_count;
static bool io_failed, reset_pending, retry_wait, reopen_pending, link_up;
static bool discarding_packet;
static uint32_t retry_started, rx_led_started, tx_led_started;
static bool rx_led_on, tx_led_on;
static uint8_t packet[BRIDGE_PACKET_MAX];
static uint8_t destination[4] = {192, 168, 1, 255};
static const wiz_NetInfo network = {
    .mac = {0x00, 0x08, 0xdc, 0xab, 0xcd, 0xef},
    .ip = {192, 168, 1, 101},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 1, 1},
    .dhcp = NETINFO_STATIC
};

uint32_t EthIo_Now(void) { return HAL_GetTick(); }
bool EthIo_Failed(void) { return io_failed; }
void EthIo_LatchFault(void) { io_failed = true; }

bool EthIo_Wait(uint32_t started, uint32_t timeout_ms)
{
    if (io_failed) return false;
    if ((uint32_t)(HAL_GetTick() - started) >= timeout_ms) {
        ++bridge_diagnostics.ethernet_timeouts;
        return false;
    }
    osDelay(1);
    return true;
}

static void select_chip(void)
{
    HAL_GPIO_WritePin(SPI1_SCS_GPIO_Port, SPI1_SCS_Pin, GPIO_PIN_RESET);
}

static void unselect_chip(void)
{
    HAL_GPIO_WritePin(SPI1_SCS_GPIO_Port, SPI1_SCS_Pin, GPIO_PIN_SET);
}

static void read_spi(uint8_t *buffer, uint16_t length)
{
    if (!length) return;
    /* Void ioLibrary callbacks cannot return errors. Latch them for every caller. */
    if (io_failed) { memset(buffer, 0, length); return; }
    if (HAL_SPI_Receive(&hspi1, buffer, length, SPI_TIMEOUT_MS) != HAL_OK) {
        memset(buffer, 0, length);
        ++bridge_diagnostics.spi_errors;
        io_failed = true;
    }
}

static void write_spi(uint8_t *buffer, uint16_t length)
{
    if (!length || io_failed) return;
    if (HAL_SPI_Transmit(&hspi1, buffer, length, SPI_TIMEOUT_MS) != HAL_OK) {
        ++bridge_diagnostics.spi_errors;
        io_failed = true;
    }
}

static uint8_t read_byte(void)
{
    uint8_t byte = 0;
    read_spi(&byte, 1);
    return byte;
}

static void write_byte(uint8_t byte) { write_spi(&byte, 1); }

static bool pop_frame(BridgeFrame *frame)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    bool available = queue_count != 0;
    if (available) {
        *frame = can_queue[queue_tail];
        queue_tail = (queue_tail + 1) % BRIDGE_CAN_QUEUE_SIZE;
        --queue_count;
    }
    __set_PRIMASK(mask);
    return available;
}

static void discard_frames(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    bridge_diagnostics.can_offline_drops += queue_count;
    queue_tail = queue_head;
    queue_count = 0;
    __set_PRIMASK(mask);
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *handle)
{
    /* bxCAN FIFO holds three frames. Bound each interrupt, even under saturation. */
    for (unsigned i = 0; i < 3 && HAL_CAN_GetRxFifoFillLevel(handle, CAN_RX_FIFO0); ++i) {
        CAN_RxHeaderTypeDef header;
        BridgeFrame frame = {0};
        if (HAL_CAN_GetRxMessage(handle, CAN_RX_FIFO0, &header, frame.data) != HAL_OK) {
            ++bridge_diagnostics.can_rx_errors;
            break;
        }
        if (header.IDE != CAN_ID_STD || header.RTR != CAN_RTR_DATA || header.DLC > 8) {
            ++bridge_diagnostics.can_rx_ignored;
            continue;
        }
        frame.id = (uint16_t)header.StdId;
        frame.dlc = (uint8_t)header.DLC;
        if (queue_count == BRIDGE_CAN_QUEUE_SIZE) {
            ++bridge_diagnostics.can_queue_overflow;
        } else {
            can_queue[queue_head] = frame;
            queue_head = (queue_head + 1) % BRIDGE_CAN_QUEUE_SIZE;
            ++queue_count;
        }
    }
}

static void monitor_can(void)
{
    uint32_t esr = hcan.Instance->ESR;
    uint32_t previous = bridge_diagnostics.can_esr;
    if ((esr ^ previous) & (CAN_ESR_EWGF | CAN_ESR_EPVF | CAN_ESR_BOFF | CAN_ESR_LEC))
        ++bridge_diagnostics.can_error_transitions;
    if ((esr & CAN_ESR_BOFF) && !(previous & CAN_ESR_BOFF)) {
        ++bridge_diagnostics.can_bus_off_entries;
        /* Automatic recovery must not replay commands pending before the fault. */
        if (HAL_CAN_AbortTxRequest(&hcan, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2) != HAL_OK)
            ++bridge_diagnostics.can_tx_errors;
    }
    if (!(esr & CAN_ESR_BOFF) && (previous & CAN_ESR_BOFF))
        ++bridge_diagnostics.can_bus_off_exits;
    bridge_diagnostics.can_esr = esr;
}

static void update_leds(void)
{
    uint32_t now = HAL_GetTick();
    if (rx_led_on && (uint32_t)(now - rx_led_started) >= 20) {
        HAL_GPIO_WritePin(LED_RX_GPIO_Port, LED_RX_Pin, GPIO_PIN_RESET);
        rx_led_on = false;
    }
    if (tx_led_on && (uint32_t)(now - tx_led_started) >= 20) {
        HAL_GPIO_WritePin(LED_3_GPIO_Port, LED_3_Pin, GPIO_PIN_RESET);
        tx_led_on = false;
    }
}

static bool initialize_ethernet(void)
{
    uint8_t sizes[8] = {16, 0, 0, 0, 0, 0, 0, 0};
    wiz_NetInfo desired = network, actual = {0};
    ++bridge_diagnostics.ethernet_resets;
    unselect_chip();
    io_failed = false;
    if (HAL_SPI_DeInit(&hspi1) != HAL_OK || HAL_SPI_Init(&hspi1) != HAL_OK) {
        ++bridge_diagnostics.spi_errors;
        io_failed = true;
        return false;
    }
    HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_RESET);
    /* One extra 1 ms RTOS tick accounts for calls just before a tick boundary. */
    osDelay(2);
    HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_SET);
    osDelay(11);
    if (getVERSIONR() != 0x04 || io_failed) return false;
    if (wizchip_init(sizes, sizes) != 0 || io_failed) return false;
    wizchip_setnetinfo(&desired);
    wizchip_getnetinfo(&actual);
    if (io_failed || memcmp(actual.mac, network.mac, 6) ||
        memcmp(actual.ip, network.ip, 4) || memcmp(actual.sn, network.sn, 4) ||
        memcmp(actual.gw, network.gw, 4)) return false;
    for (unsigned i = 0; i < 8; ++i) {
        if (getSn_TXBUF_SIZE(i) != sizes[i] || getSn_RXBUF_SIZE(i) != sizes[i] || io_failed)
            return false;
    }
    link_up = false;
    reopen_pending = true;
    discarding_packet = false;
    return true;
}

static void socket_failed(void)
{
    ++bridge_diagnostics.socket_errors;
    reopen_pending = true;
    discard_frames();
    if (io_failed) reset_pending = true;
}

static bool receive_command(void)
{
    uint8_t source[4];
    uint16_t port, remaining = 0;
    BridgeFrame frame;
    uint16_t available = getSn_RX_RSR(BRIDGE_SOCKET);
    if (io_failed) return false;
    if (!available) return true;
    int32_t received = recvfrom(BRIDGE_SOCKET, packet, sizeof(packet), source, &port);
    if (received < 0 || io_failed) return false;
    if (getsockopt(BRIDGE_SOCKET, SO_REMAINSIZE, &remaining) != SOCK_OK) return false;
    /* Drain one chunk per iteration: never parse continuations as new commands. */
    if (discarding_packet) {
        discarding_packet = remaining != 0;
        return true;
    }
    if (remaining || !Bridge_Parse(packet, (size_t)received, &frame)) {
        ++bridge_diagnostics.parser_rejections;
        discarding_packet = remaining != 0;
        return true;
    }
    memcpy(destination, source, sizeof(destination));
    if ((hcan.Instance->ESR & CAN_ESR_BOFF) || HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0) {
        ++bridge_diagnostics.can_tx_busy;
        return true;
    }
    CAN_TxHeaderTypeDef header = {0};
    uint32_t mailbox;
    header.StdId = frame.id;
    header.DLC = frame.dlc;
    header.IDE = CAN_ID_STD;
    header.RTR = CAN_RTR_DATA;
    if (HAL_CAN_AddTxMessage(&hcan, &header, frame.data, &mailbox) != HAL_OK) {
        ++bridge_diagnostics.can_tx_errors;
    } else {
        ++bridge_diagnostics.can_tx_ok;
        HAL_GPIO_WritePin(LED_3_GPIO_Port, LED_3_Pin, GPIO_PIN_SET);
        tx_led_started = HAL_GetTick();
        tx_led_on = true;
    }
    return true;
}

static bool forward_can(void)
{
    BridgeFrame frame;
    char response[42];
    if (!pop_frame(&frame)) return true;
    size_t length = Bridge_Format(&frame, response, sizeof(response));
    int32_t sent = sendto(BRIDGE_SOCKET, (uint8_t *)response, (uint16_t)length,
                          destination, BRIDGE_TX_PORT);
    if (io_failed || sent != (int32_t)length) {
        ++bridge_diagnostics.udp_send_drops;
        return !io_failed && sent == SOCK_BUSY;
    }
    ++bridge_diagnostics.can_rx_forwarded;
    HAL_GPIO_WritePin(LED_RX_GPIO_Port, LED_RX_Pin, GPIO_PIN_SET);
    rx_led_started = HAL_GetTick();
    rx_led_on = true;
    return true;
}

void Bridge_Init(void)
{
    reg_wizchip_cs_cbfunc(select_chip, unselect_chip);
    reg_wizchip_spi_cbfunc(read_byte, write_byte);
    reg_wizchip_spiburst_cbfunc(read_spi, write_spi);
    reset_pending = true;
    retry_wait = false;
}

void Bridge_Step(void)
{
    ++bridge_diagnostics.task_iterations;
    bridge_diagnostics.last_progress_ms = HAL_GetTick();
    monitor_can();
    update_leds();
    if (reset_pending) {
        discard_frames();
        if (retry_wait && (uint32_t)(HAL_GetTick() - retry_started) < RETRY_MS) return;
        if (!initialize_ethernet()) {
            ++bridge_diagnostics.ethernet_init_failures;
            retry_started = HAL_GetTick();
            retry_wait = true;
            return;
        }
        reset_pending = false;
        retry_wait = false;
        discard_frames();
    }
    uint8_t phy = getPHYCFGR();
    if (io_failed) { reset_pending = true; return; }
    if (!(phy & PHYCFGR_LNK_ON)) {
        if (link_up) ++bridge_diagnostics.link_down_events;
        link_up = false;
        reopen_pending = true;
        discard_frames();
        return;
    }
    link_up = true;
    if (reopen_pending) {
        ++bridge_diagnostics.socket_reopens;
        if (socket(BRIDGE_SOCKET, Sn_MR_UDP, BRIDGE_RX_PORT, SF_IO_NONBLOCK) != BRIDGE_SOCKET || io_failed) {
            socket_failed();
            reset_pending = true;
            retry_started = HAL_GetTick();
            retry_wait = true;
            return;
        }
        reopen_pending = false;
        discarding_packet = false;
    }
    if (getSn_SR(BRIDGE_SOCKET) != SOCK_UDP || io_failed) { socket_failed(); return; }
    if (!receive_command()) { socket_failed(); return; }
    if (!forward_can()) socket_failed();
}
