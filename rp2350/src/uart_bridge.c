// Crazyflie UART1/UART2 <-> CDC ports 1/2. See uart_bridge.h.

#include "uart_bridge.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "tusb.h"

#include "board.h"

#define RING_SIZE 2048   // power of two; ~20 ms at 1 Mbaud if the host stalls

typedef struct {
    uart_inst_t *uart;
    uint tx_pin, rx_pin;
    gpio_function_t func;
    uint irq;
    uint8_t cdc;                 // TinyUSB CDC instance
    bool on;
    bool dtr;                    // Host DTR, last seen
    cdc_line_coding_t coding;    // the host's, applied while on
    volatile uint32_t head;      // written by the IRQ
    uint32_t tail;               // written by the main loop
    volatile uint32_t dropped;
    uint8_t ring[RING_SIZE];
} port_t;

// TX drives the Crazyflie's TX pin: no crossover, see board.h.
static port_t s_ports[UART_BRIDGE_PORTS] = {
    {.uart = uart0, .tx_pin = PIN_TX1, .rx_pin = PIN_RX1, .func = GPIO_FUNC_UART,
     .irq = UART0_IRQ, .cdc = 1},
    {.uart = uart1, .tx_pin = PIN_TX2, .rx_pin = PIN_RX2, .func = GPIO_FUNC_UART_AUX,
     .irq = UART1_IRQ, .cdc = 2},
};

static void rx_irq(port_t *p) {
    uart_hw_t *hw = uart_get_hw(p->uart);
    while (uart_is_readable(p->uart)) {
        uint32_t dr = hw->dr;   // bits 8-11: FE, PE, BE, OE
        if (dr & UART_UARTDR_OE_BITS) p->dropped++;
        if (dr & (UART_UARTDR_FE_BITS | UART_UARTDR_BE_BITS)) continue;   // line noise / idle-low
        uint32_t h = p->head;
        if (h - p->tail == RING_SIZE) {
            p->dropped++;
            continue;
        }
        p->ring[h & (RING_SIZE - 1)] = (uint8_t)dr;
        p->head = h + 1;
    }
}

static void uart0_irq(void) { rx_irq(&s_ports[0]); }
static void uart1_irq(void) { rx_irq(&s_ports[1]); }

static void apply_coding(port_t *p) {
    const cdc_line_coding_t *c = &p->coding;
    uart_parity_t parity = c->parity == 1 ? UART_PARITY_ODD
                         : c->parity == 2 ? UART_PARITY_EVEN : UART_PARITY_NONE;
    uint data = c->data_bits >= 5 && c->data_bits <= 8 ? c->data_bits : 8;
    uart_set_baudrate(p->uart, c->bit_rate);
    uart_set_format(p->uart, data, c->stop_bits == 2 ? 2 : 1, parity);
}

// Back to a plain input, exactly as board_init() leaves the CF signals.
static void release(uint pin) {
    gpio_init(pin);
    gpio_disable_pulls(pin);
}

void uart_bridge_init(void) {
    irq_set_exclusive_handler(UART0_IRQ, uart0_irq);
    irq_set_exclusive_handler(UART1_IRQ, uart1_irq);
    for (uint i = 0; i < UART_BRIDGE_PORTS; i++) {
        s_ports[i].coding = (cdc_line_coding_t){.bit_rate = 115200, .stop_bits = 0,
                                                .parity = 0, .data_bits = 8};
    }
}

void uart_bridge_set(uint32_t port, bool on) {
    port_t *p = &s_ports[port];
    if (on == p->on) return;
    if (on) {
        uart_init(p->uart, p->coding.bit_rate);
        apply_coding(p);
        p->tail = p->head;
        p->dropped = 0;
        // The UART idles its TX high from here; the pin follows once it is
        // switched over.
        gpio_set_function(p->tx_pin, p->func);
        gpio_set_function(p->rx_pin, p->func);
        uart_set_irqs_enabled(p->uart, true, false);   // RX + RX timeout
        irq_set_enabled(p->irq, true);
    } else {
        irq_set_enabled(p->irq, false);
        uart_deinit(p->uart);
        release(p->tx_pin);
        release(p->rx_pin);
    }
    p->on = on;
}

bool uart_bridge_on(uint32_t port) { return s_ports[port].on; }
uint32_t uart_bridge_baud(uint32_t port) { return s_ports[port].coding.bit_rate; }
uint32_t uart_bridge_dropped(uint32_t port) { return s_ports[port].dropped; }

void uart_bridge_poll(void) {
    for (uint i = 0; i < UART_BRIDGE_PORTS; i++) {
        port_t *p = &s_ports[i];
        if (!p->on) {
            // Nobody to send it to: keep the host from blocking on a full port.
            if (tud_cdc_n_available(p->cdc)) tud_cdc_n_read_flush(p->cdc);
            continue;
        }

        // Host -> TX pin.
        while (uart_is_writable(p->uart) && tud_cdc_n_available(p->cdc)) {
            uint8_t b;
            tud_cdc_n_read(p->cdc, &b, 1);
            uart_putc_raw(p->uart, b);
        }

        // RX pin -> host, whatever the host does with DTR: bootloader tools
        // (the QCC flasher) hold it low while they use the port. What nobody
        // reads waits in the ring until it overflows, and a terminal that
        // opens the port starts afresh, see tud_cdc_line_state_cb().
        uint32_t head = p->head;
        while (p->tail != head) {
            uint32_t at = p->tail & (RING_SIZE - 1);
            uint32_t n = head - p->tail;
            if (n > RING_SIZE - at) n = RING_SIZE - at;   // up to the wrap
            n = tud_cdc_n_write(p->cdc, &p->ring[at], n);
            if (!n) break;
            p->tail += n;
        }
        tud_cdc_n_write_flush(p->cdc);
    }
}

// DTR going high is a terminal opening the port: give it what arrives from
// now on rather than what came in while nobody was reading.
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == 0 || itf > UART_BRIDGE_PORTS) return;
    port_t *p = &s_ports[itf - 1];
    if (dtr && !p->dtr) {
        p->tail = p->head;
        tud_cdc_n_write_clear(p->cdc);
    }
    p->dtr = dtr;
}

// The host opened a port or changed its settings. CDC 0 is the control
// channel, where the rate means nothing.
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
    if (itf == 0 || itf > UART_BRIDGE_PORTS) return;
    port_t *p = &s_ports[itf - 1];
    if (!coding->bit_rate) return;
    p->coding = *coding;
    if (p->on) apply_coding(p);
}
