#include "bridge.h"
#include "bridge_protocol.h"
#include "eth_io.h"
#include "can.h"
#include "spi.h"
#include "socket.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Exercise the production bridge AND ioLibrary through an emulated SPI bus. */
static uint8_t registers[32][65536];
static uint32_t tick, irq_mask;
static unsigned spi_phase, spi_block;
static uint16_t spi_address, rx_producer, tx_consumed;
static bool phy_link = true;
static struct {
    bool spi, version, network, open_status, close_status, send_silent, send_timeout;
    bool unstable_rx, unstable_tx;
    uint8_t stuck_command, spi_on_command;
    unsigned stuck_recv_number;
} fault;
static unsigned unstable_reads, can_pending, can_submitted, send_count, recv_commands;
static unsigned can_aborts;
static unsigned free_mailboxes = 3;
static HAL_StatusTypeDef can_result = HAL_OK;
static CAN_RxHeaderTypeDef injected_header;
static uint8_t injected_data[8], last_can_data[8], last_destination[4];
static CAN_TxHeaderTypeDef last_can_header;
static char last_response[100];
static CAN_TypeDef can_registers;
CAN_HandleTypeDef hcan = {&can_registers};
SPI_HandleTypeDef hspi1;

static uint8_t *reg(uint32_t address)
{
    return &registers[(address >> 3) & 31][address >> 8];
}

static uint16_t read16(uint32_t address)
{
    return (uint16_t)((reg(address)[0] << 8) | reg(address)[1]);
}

static void write16(uint32_t address, uint16_t value)
{
    reg(address)[0] = (uint8_t)(value >> 8);
    reg(address)[1] = (uint8_t)value;
}

static void reset_chip(void)
{
    memset(registers, 0, sizeof(registers));
    *reg(VERSIONR) = fault.version ? 0 : 4;
    *reg(PHYCFGR) = phy_link ? PHYCFGR_LNK_ON : 0;
    rx_producer = tx_consumed = 0;
    recv_commands = 0;
}

uint32_t HAL_GetTick(void) { return tick; }
void osDelay(uint32_t milliseconds) { tick += milliseconds; }
uint32_t __get_PRIMASK(void) { return irq_mask; }
void __disable_irq(void) { irq_mask = 1; }
void __set_PRIMASK(uint32_t mask) { irq_mask = mask; }

void HAL_GPIO_WritePin(unsigned port, unsigned pin, GPIO_PinState state)
{
    (void)port;
    if (pin == SPI1_SCS_Pin && state == GPIO_PIN_RESET) spi_phase = 0;
    if (pin == ETH_RST_Pin && state == GPIO_PIN_RESET) reset_chip();
}

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *handle) { (void)handle; return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *handle) { (void)handle; return HAL_OK; }

static void register_write(uint8_t value)
{
    uint32_t address = ((uint32_t)spi_address << 8) | (spi_block << 3);
    if (address == MR && (value & MR_RST)) { reset_chip(); return; }
    if (address == Sn_IR(0)) { *reg(address) &= (uint8_t)~value; return; }
    *reg(address) = value;
    if (address == Sn_TXBUF_SIZE(0)) write16(Sn_TX_FSR(0), (uint16_t)(value * 1024));
    if (address != Sn_CR(0)) return;
    if (value == Sn_CR_RECV && ++recv_commands == fault.stuck_recv_number) return;
    if (value == fault.stuck_command) return;
    if (value == fault.spi_on_command) fault.spi = true;
    *reg(address) = 0;
    if (value == Sn_CR_OPEN && !fault.open_status) *reg(Sn_SR(0)) = SOCK_UDP;
    if (value == Sn_CR_CLOSE && !fault.close_status) {
        *reg(Sn_SR(0)) = SOCK_CLOSED;
        write16(Sn_RX_RD(0), 0);
        write16(Sn_RX_RSR(0), 0);
        write16(Sn_TX_WR(0), 0);
        rx_producer = tx_consumed = 0;
    }
    if (value == Sn_CR_RECV)
        write16(Sn_RX_RSR(0), (uint16_t)(rx_producer - read16(Sn_RX_RD(0))));
    if (value == Sn_CR_SEND) {
        uint16_t end = read16(Sn_TX_WR(0));
        unsigned length = (uint16_t)(end - tx_consumed);
        assert(length < sizeof(last_response));
        for (unsigned i = 0; i < length; ++i)
            last_response[i] = (char)registers[WIZCHIP_TXBUF_BLOCK(0)][(tx_consumed + i) & 0x3fff];
        last_response[length] = 0;
        memcpy(last_destination, reg(Sn_DIPR(0)), 4);
        tx_consumed = end;
        ++send_count;
        if (!fault.send_silent)
            *reg(Sn_IR(0)) |= fault.send_timeout ? Sn_IR_TIMEOUT : Sn_IR_SENDOK;
    }
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *handle, uint8_t *buffer,
                                  uint16_t length, uint32_t timeout)
{
    (void)handle;
    assert(timeout == 10);
    if (fault.spi) { tick += timeout; return HAL_TIMEOUT; }
    for (unsigned i = 0; i < length; ++i) {
        if (spi_phase == 0) spi_address = (uint16_t)(buffer[i] << 8);
        else if (spi_phase == 1) spi_address |= buffer[i];
        else if (spi_phase == 2) spi_block = buffer[i] >> 3;
        else { register_write(buffer[i]); ++spi_address; }
        ++spi_phase;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *handle, uint8_t *buffer,
                                 uint16_t length, uint32_t timeout)
{
    (void)handle;
    assert(timeout == 10);
    if (fault.spi) { tick += timeout; return HAL_TIMEOUT; }
    for (unsigned i = 0; i < length; ++i) {
        uint32_t address = ((uint32_t)spi_address << 8) | (spi_block << 3);
        buffer[i] = registers[spi_block][spi_address];
        if (spi_block == WIZCHIP_RXBUF_BLOCK(0))
            buffer[i] = registers[spi_block][spi_address & 0x3fff];
        if ((fault.unstable_rx && address == WIZCHIP_OFFSET_INC(Sn_RX_RSR(0), 1)) ||
            (fault.unstable_tx && address == WIZCHIP_OFFSET_INC(Sn_TX_FSR(0), 1)))
            buffer[i] = (uint8_t)(++unstable_reads & 1);
        if (fault.network && address == SIPR) buffer[i] ^= 1;
        ++spi_address;
    }
    return HAL_OK;
}

uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *handle, uint32_t fifo)
{
    (void)handle; (void)fifo;
    return can_pending;
}

HAL_StatusTypeDef HAL_CAN_GetRxMessage(CAN_HandleTypeDef *handle, uint32_t fifo,
                                      CAN_RxHeaderTypeDef *header, uint8_t *data)
{
    (void)handle; (void)fifo;
    assert(can_pending);
    --can_pending;
    *header = injected_header;
    memcpy(data, injected_data, 8);
    return HAL_OK;
}

uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *handle)
{
    (void)handle;
    return free_mailboxes;
}

HAL_StatusTypeDef HAL_CAN_AddTxMessage(CAN_HandleTypeDef *handle,
                                     CAN_TxHeaderTypeDef *header, uint8_t *data, uint32_t *mailbox)
{
    (void)handle;
    last_can_header = *header;
    memcpy(last_can_data, data, 8);
    *mailbox = 1;
    if (can_result == HAL_OK) ++can_submitted;
    return can_result;
}

HAL_StatusTypeDef HAL_CAN_AbortTxRequest(CAN_HandleTypeDef *handle, uint32_t mailboxes)
{
    (void)handle;
    assert(mailboxes == 7);
    ++can_aborts;
    return HAL_OK;
}

static void fixture(void)
{
    memset(&fault, 0, sizeof(fault));
    memset((void *)&bridge_diagnostics, 0, sizeof(bridge_diagnostics));
    memset(&can_registers, 0, sizeof(can_registers));
    tick = 0;
    can_pending = can_submitted = send_count = unstable_reads = can_aborts = 0;
    free_mailboxes = 3;
    can_result = HAL_OK;
    phy_link = true;
    reset_chip();
    Bridge_Init();
    Bridge_Step();
    assert(bridge_diagnostics.ethernet_resets == 1);
    assert(*reg(Sn_SR(0)) == SOCK_UDP);
}

static void push_udp(const void *data, size_t length, uint8_t source_last)
{
    uint8_t header[8] = {192, 168, 1, source_last, 0xDE, 0xAD,
                         (uint8_t)(length >> 8), (uint8_t)length};
    assert(length + read16(Sn_RX_RSR(0)) + 8 < 16384);
    for (unsigned i = 0; i < sizeof(header); ++i)
        registers[WIZCHIP_RXBUF_BLOCK(0)][rx_producer++ & 0x3fff] = header[i];
    for (size_t i = 0; i < length; ++i)
        registers[WIZCHIP_RXBUF_BLOCK(0)][rx_producer++ & 0x3fff] = ((const uint8_t *)data)[i];
    write16(Sn_RX_RSR(0), (uint16_t)(rx_producer - read16(Sn_RX_RD(0))));
}

static void push_can(uint16_t id, uint8_t dlc)
{
    injected_header = (CAN_RxHeaderTypeDef){.StdId = id, .DLC = dlc};
    memset(injected_data, 0xA5, sizeof(injected_data));
    can_pending = 1;
    HAL_CAN_RxFifo0MsgPendingCallback(&hcan);
}

static const char valid[] = "{007,232,001,7FF,8,0123456789ABCDEF}";

static void test_protocol(void)
{
    BridgeFrame frame = {0};
    char output[42];
    assert(Bridge_Parse((const uint8_t *)valid, strlen(valid), &frame));
    assert(frame.id == 0x7ff && frame.dlc == 8 && frame.data[7] == 0xef);
    assert(Bridge_Format(&frame, output, sizeof(output)) == 41);
    assert(!strcmp(output, "{507,232,4,7FF,8,01,23,45,67,89,AB,CD,EF}"));
    const char *good[] = {"{007,232,001,0,0,}", "{007,232,001,01,1,aB}",
                         " \r\n{007,232,001,0x200,2,ABcd}\n"};
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); ++i)
        assert(Bridge_Parse((const uint8_t *)good[i], strlen(good[i]), &frame));
    frame = (BridgeFrame){.id = 1, .dlc = 1, .data = {0xAB, 0xFF}};
    assert(Bridge_Format(&frame, output, sizeof(output)) == 40);
    assert(!strcmp(output, "{507,232,4,01,8,AB,00,00,00,00,00,00,00}"));
    assert(!Bridge_Format(&frame, output, 40));
    const char *bad[] = {"", "{", "{}", "007,232,001,200,0,", "{007}",
        "{007,232,001,800,0,}", "{007,232,001,-1,0,}", "{007,232,001,200,9,00}",
        "{007,232,001,200,-1,00}", "{007,232,001,200,1,0G}", "{007,232,001,200,2,00}",
        "{007,232,001,200,1,0000}", "{007,232,001,200,0,00}", "{007,232,001,200,1,}",
        "{007,232,001,200,0,,}", "{007,232,001,200,1,AA,BB}", "{007,232,002,200,0,}",
        "{007,232,001,200,99999999999999999999,00}", "{007,232,001,FFFFFFFFFFFFFFFF,0,}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        BridgeFrame unchanged = frame;
        assert(!Bridge_Parse((const uint8_t *)bad[i], strlen(bad[i]), &frame));
        assert(!memcmp(&frame, &unchanged, sizeof(frame)));
    }
    uint8_t bytes[512];
    memset(bytes, 'X', sizeof(bytes));
    assert(!Bridge_Parse(bytes, sizeof(bytes), &frame));
    for (size_t n = 0; n < strlen(valid); ++n)
        assert(!Bridge_Parse((const uint8_t *)valid, n, &frame));
    /* Deterministic malformed-input sweep, including non-NUL-terminated buffers. */
    uint32_t random = 1;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        for (unsigned j = 0; j < sizeof(bytes); ++j) {
            random = random * 1664525U + 1013904223U;
            bytes[j] = (uint8_t)(random >> 24);
        }
        (void)Bridge_Parse(bytes, iteration % sizeof(bytes), &frame);
    }
}

static void test_fairness_and_queue(void)
{
    fixture();
    push_can(1, 8);
    Bridge_Step();
    assert(last_destination[3] == 255); /* broadcast before first valid command */
    send_count = 0;
    for (unsigned i = 0; i < 100; ++i) {
        push_can((uint16_t)i, 1);
        push_udp(valid, strlen(valid), 42);
        Bridge_Step();
    }
    assert(can_submitted == 100 && send_count == 100);
    assert(last_destination[3] == 42);
    assert(last_can_header.StdId == 0x7FF && last_can_data[7] == 0xEF);
    assert(strstr(last_response, "A5,00,00,00,00,00,00,00}"));
    push_udp("{bad}", 5, 99);
    push_can(0x200, 8);
    Bridge_Step();
    assert(last_destination[3] == 42 && bridge_diagnostics.parser_rejections == 1);
    for (unsigned i = 0; i < 33; ++i) push_can((uint16_t)i, 8);
    assert(bridge_diagnostics.can_queue_overflow == 1);
    for (unsigned i = 0; i < 32; ++i) {
        Bridge_Step();
        char expected[20];
        snprintf(expected, sizeof(expected), "{507,232,4,%02X,8,", i);
        assert(!strncmp(last_response, expected, strlen(expected)));
    }
    assert(irq_mask == 0);
}

static void test_analyzer_enable_compatibility(void)
{
    /* Current analyzer uses DLC 4; released versions declared 5 with four bytes. */
    static const struct {
        const char *packet;
        uint8_t dlc;
        uint8_t fifth;
    } commands[] = {
        {"{007,232,001,201,4,1445650a}", 4, 0},
        {"{007,232,001,201,5,1445650a}", 5, 0},
        {"{007,232,001,201,5,1445650A}", 5, 0},
        {"{007,232,001,201,5,1445650a7F}", 5, 0x7F},
    };
    const uint8_t enable[] = {0x14, 0x45, 0x65, 0x0A};
    fixture();
    /* Leave nonzero data from a preceding parameter command: no stale padding. */
    const char parameters[] = "{007,232,001,201,5,13041A01F4}";
    push_udp(parameters, strlen(parameters), 42);
    Bridge_Step();
    assert(can_submitted == 1 && last_can_data[4] == 0xF4);
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        push_udp(commands[i].packet, strlen(commands[i].packet), 42);
        Bridge_Step();
        assert(can_submitted == i + 2);
        assert(bridge_diagnostics.parser_rejections == 0);
        assert(last_can_header.StdId == 0x201 && last_can_header.DLC == commands[i].dlc);
        assert(!memcmp(last_can_data, enable, sizeof(enable)));
        assert(last_can_data[4] == commands[i].fifth);
    }
    const char *invalid[] = {
        "{007,232,001,201,5,144565a0}", /* no general short-command padding */
        "{007,232,001,201,5,13041A01}", /* truncated settings */
        "{007,232,001,201,5,1445640a}", /* wrong enable key */
        "{007,232,001,201,5,1445650g}",
        "{007,232,001,201,5,1445650}",
        "{007,232,001,201,5,1445650a0}",
        "{007,232,001,201,6,1445650a}",
    };
    unsigned submitted = can_submitted;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        BridgeFrame frame = {.id = 0x123, .dlc = 1, .data = {0xAB}};
        BridgeFrame unchanged = frame;
        assert(!Bridge_Parse((const uint8_t *)invalid[i], strlen(invalid[i]), &frame));
        assert(!memcmp(&frame, &unchanged, sizeof(frame)));
        push_udp(invalid[i], strlen(invalid[i]), 99);
        push_can(0x301, 8);
        Bridge_Step();
        assert(can_submitted == submitted);
        assert(bridge_diagnostics.parser_rejections == i + 1);
        assert(last_destination[3] == 42);
    }
}

static void test_datagram_drain(void)
{
    fixture();
    uint8_t oversized[600];
    memset(oversized, 'X', sizeof(oversized));
    memcpy(oversized + 200, valid, strlen(valid));
    push_udp(oversized, sizeof(oversized), 99);
    push_udp(valid, strlen(valid), 42);
    for (unsigned i = 0; i < 3; ++i) {
        push_can(1, 8);
        Bridge_Step();
        assert(can_submitted == 0); /* continuation never executed */
    }
    Bridge_Step();
    assert(can_submitted == 1 && bridge_diagnostics.parser_rejections == 1);
    push_udp("", 0, 99);
    push_udp("x", 1, 99);
    Bridge_Step();
    Bridge_Step();
    assert(bridge_diagnostics.parser_rejections == 3);
    assert(read16(Sn_RX_RSR(0)) == 0);
}

static void test_socket_timeouts(void)
{
    uint8_t data[8] = {0}, address[4] = {192, 168, 1, 42};
    uint16_t port;
    const uint8_t commands[] = {Sn_CR_CLOSE, Sn_CR_OPEN, Sn_CR_SEND, Sn_CR_RECV};
    for (unsigned i = 0; i < sizeof(commands); ++i) {
        fixture();
        fault.stuck_command = commands[i];
        uint32_t started = tick;
        int32_t result;
        if (commands[i] == Sn_CR_CLOSE) result = close(0);
        else if (commands[i] == Sn_CR_OPEN) result = socket(0, Sn_MR_UDP, 56800, SF_IO_NONBLOCK);
        else if (commands[i] == Sn_CR_SEND) result = sendto(0, data, 8, address, 56801);
        else {
            push_udp(valid, strlen(valid), 42);
            result = recvfrom(0, data, sizeof(data), address, &port);
        }
        assert(result == SOCKERR_TIMEOUT);
        assert((uint32_t)(tick - started) == 100);
    }
    fixture();
    fault.open_status = true;
    assert(socket(0, Sn_MR_UDP, 56800, SF_IO_NONBLOCK) == SOCKERR_TIMEOUT);
    fixture();
    fault.close_status = true;
    assert(close(0) == SOCKERR_TIMEOUT);
    fixture();
    fault.send_silent = true;
    tick = UINT32_MAX - 20; /* deadline must work across tick rollover */
    uint32_t started = tick;
    assert(sendto(0, data, 8, address, 56801) == SOCKERR_TIMEOUT);
    assert((uint32_t)(tick - started) == 2000);
    fixture();
    fault.send_timeout = true;
    assert(sendto(0, data, 8, address, 56801) == SOCKERR_TIMEOUT);
    fixture();
    assert(socket(8, Sn_MR_UDP, 56800, 0) == SOCKERR_SOCKNUM);
    assert(socket(0, Sn_MR_UDP, 56800, 0) == 0);
    write16(Sn_TX_FSR(0), 0);
    started = tick;
    assert(sendto(0, data, 8, address, 56801) == SOCKERR_TIMEOUT);
    assert(tick - started == 100);
    started = tick;
    assert(recvfrom(0, data, 8, address, &port) == SOCKERR_TIMEOUT);
    assert(tick - started == 100);
    fixture();
    fault.unstable_rx = true;
    started = tick;
    assert(getSn_RX_RSR(0) == 0 && EthIo_Failed());
    assert(tick - started == 100);
    fixture();
    fault.unstable_tx = true;
    assert(getSn_TX_FSR(0) == 0 && EthIo_Failed());
    fixture();
    fault.stuck_recv_number = 2; /* final RECV command after the payload copy */
    push_udp(valid, strlen(valid), 42);
    started = tick;
    assert(recvfrom(0, data, 8, address, &port) == SOCKERR_TIMEOUT);
    assert(tick - started == 100);
    for (unsigned i = 0; i < sizeof(commands); ++i) {
        fixture();
        fault.spi_on_command = commands[i];
        int32_t result;
        if (commands[i] == Sn_CR_CLOSE) result = close(0);
        else if (commands[i] == Sn_CR_OPEN) result = socket(0, Sn_MR_UDP, 56800, SF_IO_NONBLOCK);
        else if (commands[i] == Sn_CR_SEND) result = sendto(0, data, 8, address, 56801);
        else {
            push_udp(valid, strlen(valid), 42);
            result = recvfrom(0, data, sizeof(data), address, &port);
        }
        /* Failed reads return zero, but must never be mistaken for command success. */
        assert(result == SOCKERR_TIMEOUT && EthIo_Failed());
    }
}

static void test_recovery(void)
{
    fixture();
    fault.send_silent = true;
    push_can(1, 8);
    Bridge_Step();
    assert(bridge_diagnostics.udp_send_drops == 1);
    fault.send_silent = false;
    Bridge_Step();
    assert(bridge_diagnostics.socket_reopens == 2 && bridge_diagnostics.ethernet_resets == 1);
    *reg(Sn_SR(0)) = SOCK_CLOSED;
    Bridge_Step();
    fault.stuck_command = Sn_CR_OPEN;
    Bridge_Step();
    fault.stuck_command = 0;
    tick += 1000;
    Bridge_Step();
    assert(bridge_diagnostics.ethernet_resets == 2);
    push_can(1, 8);
    Bridge_Step();
    assert(bridge_diagnostics.can_rx_forwarded == 1);

    fixture();
    fault.spi = true;
    Bridge_Step();
    assert(bridge_diagnostics.spi_errors == 1);
    Bridge_Step(); /* failed reset */
    uint32_t attempts = bridge_diagnostics.ethernet_resets;
    for (unsigned i = 0; i < 999; ++i) { ++tick; Bridge_Step(); }
    assert(bridge_diagnostics.ethernet_resets == attempts);
    fault.spi = false;
    ++tick;
    Bridge_Step();
    assert(bridge_diagnostics.ethernet_resets == attempts + 1 && !EthIo_Failed());
    push_udp(valid, strlen(valid), 42);
    Bridge_Step();
    assert(can_submitted == 1);

    fixture();
    phy_link = false;
    *reg(PHYCFGR) = 0;
    for (unsigned i = 0; i < 100; ++i) { push_can(1, 8); Bridge_Step(); }
    assert(bridge_diagnostics.ethernet_resets == 1 && bridge_diagnostics.link_down_events == 1);
    assert(bridge_diagnostics.can_offline_drops == 100 && send_count == 0);
    phy_link = true;
    *reg(PHYCFGR) = PHYCFGR_LNK_ON;
    Bridge_Step();
    push_can(1, 8);
    Bridge_Step();
    assert(send_count == 1 && bridge_diagnostics.socket_reopens == 2);

    fixture();
    fault.version = true;
    Bridge_Init();
    Bridge_Step();
    assert(bridge_diagnostics.ethernet_init_failures == 1);
    fault.version = false;
    fault.network = true;
    tick += 1000;
    Bridge_Step();
    assert(bridge_diagnostics.ethernet_init_failures == 2);
    fault.network = false;
    tick += 1000;
    Bridge_Step();
    assert(*reg(Sn_SR(0)) == SOCK_UDP);
}

static void test_can_errors_and_busy(void)
{
    fixture();
    free_mailboxes = 0;
    push_udp(valid, strlen(valid), 42);
    Bridge_Step();
    assert(bridge_diagnostics.can_tx_busy == 1 && can_submitted == 0);
    free_mailboxes = 3;
    can_result = HAL_ERROR;
    push_udp(valid, strlen(valid), 42);
    Bridge_Step();
    assert(bridge_diagnostics.can_tx_errors == 1 && can_submitted == 0);
    can_registers.ESR = CAN_ESR_BOFF;
    Bridge_Step();
    Bridge_Step();
    assert(bridge_diagnostics.can_bus_off_entries == 1);
    assert(can_aborts == 1);
    can_registers.ESR = 0;
    Bridge_Step();
    assert(bridge_diagnostics.can_bus_off_exits == 1);
    write16(Sn_TX_FSR(0), 0);
    push_can(1, 8);
    Bridge_Step();
    assert(bridge_diagnostics.udp_send_drops == 1 && bridge_diagnostics.socket_errors == 0);
}

int main(void)
{
    test_protocol();
    test_fairness_and_queue();
    test_analyzer_enable_compatibility();
    test_datagram_drain();
    test_socket_timeouts();
    test_recovery();
    test_can_errors_and_busy();
    puts("PASS: protocol, queue/fairness, datagram drain, deadlines, recovery, CAN errors");
    return 0;
}
