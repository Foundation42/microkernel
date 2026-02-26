#include "midi_hal.h"
#include <unistd.h>
#include <string.h>

/* ── RX ring buffer ──────────────────────────────────────────────── */

#define RX_RING_SIZE 256

static uint8_t s_rx_ring[RX_RING_SIZE];
static int     s_rx_head;  /* write position */
static int     s_rx_tail;  /* read position */

/* ── TX capture buffer ───────────────────────────────────────────── */

#define TX_BUF_SIZE 1024

static uint8_t s_tx_buf[TX_BUF_SIZE];
static int     s_tx_len;

/* ── State ───────────────────────────────────────────────────────── */

static int  s_pipe_read_fd  = -1;
static int  s_pipe_write_fd = -1;
static bool s_configured;

/* ── HAL implementation ──────────────────────────────────────────── */

bool midi_hal_init(void) {
    s_rx_head = s_rx_tail = 0;
    s_tx_len = 0;
    s_configured = false;

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return false;
    s_pipe_read_fd  = pipefd[0];
    s_pipe_write_fd = pipefd[1];
    return true;
}

void midi_hal_deinit(void) {
    if (s_pipe_read_fd >= 0) { close(s_pipe_read_fd); s_pipe_read_fd = -1; }
    if (s_pipe_write_fd >= 0) { close(s_pipe_write_fd); s_pipe_write_fd = -1; }
    s_configured = false;
}

bool midi_hal_configure(int i2c_port, uint8_t i2c_addr,
                        int sda, int scl, int irq, int rst,
                        uint32_t i2c_freq) {
    (void)i2c_port; (void)i2c_addr;
    (void)sda; (void)scl; (void)irq; (void)rst;
    (void)i2c_freq;
    s_configured = true;
    return true;
}

void midi_hal_deconfigure(void) {
    s_configured = false;
}

int midi_hal_get_notify_fd(void) {
    return s_pipe_read_fd;
}

int midi_hal_drain_rx(uint8_t *buf, int max) {
    /* Drain the pipe (just to clear the POLLIN notification) */
    uint8_t dummy[64];
    (void)read(s_pipe_read_fd, dummy, sizeof(dummy));

    /* Read from ring buffer */
    int count = 0;
    while (count < max && s_rx_tail != s_rx_head) {
        buf[count++] = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1) % RX_RING_SIZE;
    }
    return count;
}

int midi_hal_tx(const uint8_t *data, size_t len) {
    if (!s_configured)
        return -1;

    /* Capture TX data for test verification */
    for (size_t i = 0; i < len && s_tx_len < TX_BUF_SIZE; i++)
        s_tx_buf[s_tx_len++] = data[i];

    return 0;
}

bool midi_hal_read_status(midi_hal_status_t *out) {
    memset(out, 0, sizeof(*out));
    return s_configured;
}

/* ── Test helpers ─────────────────────────────────────────────────── */

void midi_mock_inject_rx(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int next = (s_rx_head + 1) % RX_RING_SIZE;
        if (next == s_rx_tail)
            break;  /* ring full */
        s_rx_ring[s_rx_head] = data[i];
        s_rx_head = next;
    }

    /* Signal the notification pipe */
    if (s_pipe_write_fd >= 0) {
        uint8_t b = 1;
        (void)write(s_pipe_write_fd, &b, 1);
    }
}

int midi_mock_get_tx(uint8_t *buf, int max) {
    int n = s_tx_len < max ? s_tx_len : max;
    memcpy(buf, s_tx_buf, (size_t)n);
    return n;
}

void midi_mock_clear_tx(void) {
    s_tx_len = 0;
}

bool midi_mock_is_configured(void) {
    return s_configured;
}
