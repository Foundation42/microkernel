#include "gpio_hal.h"
#include <esp_attr.h>
#include <driver/gpio.h>
#include <esp_vfs_eventfd.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

/* ── Notification: ring buffer + eventfd ──────────────────────────── */

#define RING_SIZE 32

static int s_notify_fd = -1;
static volatile uint8_t s_ring[RING_SIZE];
static volatile int s_ring_head = 0;
static volatile int s_ring_tail = 0;

static bool s_isr_service_installed = false;

/* ISR handler: write pin to ring buffer, signal eventfd */
static void IRAM_ATTR gpio_isr_handler(void *arg) {
    int pin = (int)(intptr_t)arg;

    int next = (s_ring_head + 1) % RING_SIZE;
    if (next != s_ring_tail) {
        s_ring[s_ring_head] = (uint8_t)pin;
        s_ring_head = next;
    }

    /* Signal eventfd to wake poll loop */
    uint64_t val = 1;
    write(s_notify_fd, &val, sizeof(val));
}

/* ── HAL interface ────────────────────────────────────────────────── */

bool gpio_hal_init(void) {
    s_ring_head = 0;
    s_ring_tail = 0;

    s_notify_fd = eventfd(0, 0);
    if (s_notify_fd < 0) return false;

    /* Set non-blocking */
    int flags = fcntl(s_notify_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(s_notify_fd, F_SETFL, flags | O_NONBLOCK);

    if (!s_isr_service_installed) {
        if (gpio_install_isr_service(0) != ESP_OK) {
            close(s_notify_fd);
            s_notify_fd = -1;
            return false;
        }
        s_isr_service_installed = true;
    }

    return true;
}

void gpio_hal_deinit(void) {
    if (s_isr_service_installed) {
        gpio_uninstall_isr_service();
        s_isr_service_installed = false;
    }
    if (s_notify_fd >= 0) {
        close(s_notify_fd);
        s_notify_fd = -1;
    }
}

int gpio_hal_get_notify_fd(void) {
    return s_notify_fd;
}

int gpio_hal_drain_events(uint8_t *pins, int max) {
    /* Read and clear the eventfd counter */
    uint64_t val;
    read(s_notify_fd, &val, sizeof(val));

    /* Drain ring buffer */
    int count = 0;
    while (s_ring_tail != s_ring_head && count < max) {
        pins[count++] = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
    }
    return count;
}

bool gpio_hal_configure(int pin, int mode) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    switch (mode) {
    case 0: /* input */
        cfg.mode = GPIO_MODE_INPUT;
        break;
    case 1: /* output */
        cfg.mode = GPIO_MODE_OUTPUT;
        break;
    case 2: /* input_pullup */
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        break;
    case 3: /* input_pulldown */
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        break;
    default:
        return false;
    }

    return gpio_config(&cfg) == ESP_OK;
}

bool gpio_hal_write(int pin, int value) {
    return gpio_set_level((gpio_num_t)pin, value) == ESP_OK;
}

int gpio_hal_read(int pin) {
    return gpio_get_level((gpio_num_t)pin);
}

bool gpio_hal_isr_add(int pin, int edge) {
    gpio_int_type_t intr_type;
    switch (edge) {
    case 0:  intr_type = GPIO_INTR_POSEDGE; break;
    case 1:  intr_type = GPIO_INTR_NEGEDGE; break;
    default: intr_type = GPIO_INTR_ANYEDGE; break;
    }

    gpio_set_intr_type((gpio_num_t)pin, intr_type);

    if (gpio_isr_handler_add((gpio_num_t)pin, gpio_isr_handler,
                              (void *)(intptr_t)pin) != ESP_OK)
        return false;

    return gpio_intr_enable((gpio_num_t)pin) == ESP_OK;
}

bool gpio_hal_isr_remove(int pin) {
    gpio_isr_handler_remove((gpio_num_t)pin);
    gpio_intr_disable((gpio_num_t)pin);
    return true;
}
