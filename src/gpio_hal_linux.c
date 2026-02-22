#include "gpio_hal.h"
#include <unistd.h>
#include <string.h>

#define MAX_MOCK_PINS 64

static int mock_values[MAX_MOCK_PINS];
static int mock_modes[MAX_MOCK_PINS];   /* -1 = unconfigured */
static bool mock_isr_active[MAX_MOCK_PINS];

static int s_pipe_read_fd  = -1;
static int s_pipe_write_fd = -1;

bool gpio_hal_init(void) {
    memset(mock_values, 0, sizeof(mock_values));
    memset(mock_isr_active, 0, sizeof(mock_isr_active));
    for (int i = 0; i < MAX_MOCK_PINS; i++)
        mock_modes[i] = -1;

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return false;
    s_pipe_read_fd  = pipefd[0];
    s_pipe_write_fd = pipefd[1];
    return true;
}

void gpio_hal_deinit(void) {
    if (s_pipe_read_fd >= 0) { close(s_pipe_read_fd); s_pipe_read_fd = -1; }
    if (s_pipe_write_fd >= 0) { close(s_pipe_write_fd); s_pipe_write_fd = -1; }
}

int gpio_hal_get_notify_fd(void) {
    return s_pipe_read_fd;
}

int gpio_hal_drain_events(uint8_t *pins, int max) {
    if (s_pipe_read_fd < 0) return -1;
    ssize_t n = read(s_pipe_read_fd, pins, (size_t)max);
    return (int)n;
}

bool gpio_hal_configure(int pin, int mode) {
    if (pin < 0 || pin >= MAX_MOCK_PINS) return false;
    mock_modes[pin] = mode;
    return true;
}

bool gpio_hal_write(int pin, int value) {
    if (pin < 0 || pin >= MAX_MOCK_PINS) return false;
    if (mock_modes[pin] != 1) return false; /* must be output */
    mock_values[pin] = value ? 1 : 0;
    return true;
}

int gpio_hal_read(int pin) {
    if (pin < 0 || pin >= MAX_MOCK_PINS) return -1;
    return mock_values[pin];
}

bool gpio_hal_isr_add(int pin, int edge) {
    (void)edge;
    if (pin < 0 || pin >= MAX_MOCK_PINS) return false;
    mock_isr_active[pin] = true;
    return true;
}

bool gpio_hal_isr_remove(int pin) {
    if (pin < 0 || pin >= MAX_MOCK_PINS) return false;
    mock_isr_active[pin] = false;
    return true;
}

/* ── Test helpers ─────────────────────────────────────────────────── */

void gpio_mock_trigger_interrupt(int pin, int value) {
    if (pin < 0 || pin >= MAX_MOCK_PINS) return;
    mock_values[pin] = value ? 1 : 0;
    if (s_pipe_write_fd >= 0) {
        uint8_t b = (uint8_t)pin;
        (void)write(s_pipe_write_fd, &b, 1);
    }
}

void gpio_mock_set_value(int pin, int value) {
    if (pin >= 0 && pin < MAX_MOCK_PINS)
        mock_values[pin] = value ? 1 : 0;
}
