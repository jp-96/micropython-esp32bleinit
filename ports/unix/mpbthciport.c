/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jim Mussared
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#if MICROPY_PY_BLUETOOTH && (MICROPY_BLUETOOTH_NIMBLE || (MICROPY_BLUETOOTH_BTSTACK && MICROPY_BLUETOOTH_BTSTACK_H4))

#if !MICROPY_PY_THREAD
#error Unix HCI UART requires MICROPY_PY_THREAD
#endif

#include "extmod/modbluetooth.h"
#include "extmod/mpbthci.h"

#include <pthread.h>
#include <unistd.h>

#include <termios.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG_printf(...) // printf(__VA_ARGS__)
#define DEBUG_HCI_DUMP (0)

uint8_t mp_bluetooth_hci_cmd_buf[4 + 256];

// Must be provided by the stack bindings (e.g. mpnimbleport.c or mpbtstackport.c).
extern bool mp_bluetooth_hci_poll(void);

STATIC const useconds_t UART_POLL_INTERVAL_US = 1000;

STATIC int uart_fd = -1;
STATIC pthread_t hci_poll_thread_id;

STATIC void *hci_poll_thread(void *arg) {
    (void)arg;

    // This will return false when the stack is shutdown.
    while (mp_bluetooth_hci_poll()) {
        usleep(UART_POLL_INTERVAL_US);
    }

    return NULL;
}

STATIC int configure_uart(void) {
    struct termios toptions;

    // Get existing config.
    if (tcgetattr(uart_fd, &toptions) < 0) {
        DEBUG_printf("Couldn't get term attributes");
        return -1;
    }

    // Raw mode (disable all processing).
    cfmakeraw(&toptions);

    // 8N1, no parity.
    toptions.c_cflag &= ~CSTOPB;
    toptions.c_cflag |= CS8;
    toptions.c_cflag &= ~PARENB;

    // Enable receiver, ignore modem control lines
    toptions.c_cflag |= CREAD | CLOCAL;

    // Blocking, single-byte reads.
    toptions.c_cc[VMIN] = 1;
    toptions.c_cc[VTIME] = 0;

    // Enable HW RTS/CTS flow control.
    toptions.c_iflag &= ~(IXON | IXOFF | IXANY);
    toptions.c_cflag |= CRTSCTS;

    // 1Mbit (TODO: make this configurable).
    speed_t brate = B1000000;
    cfsetospeed(&toptions, brate);
    cfsetispeed(&toptions, brate);

    // Apply immediately.
    if (tcsetattr(uart_fd, TCSANOW, &toptions) < 0) {
        DEBUG_printf("Couldn't set term attributes");

        close(uart_fd);
        uart_fd = -1;

        return -1;
    }

    return 0;
}

// HCI UART bindings.
int mp_bluetooth_hci_uart_init(uint32_t port, uint32_t baudrate) {
    (void)port;
    (void)baudrate;

    DEBUG_printf("mp_bluetooth_hci_uart_init (unix)\n");

    char uart_device_name[256] = "/dev/ttyUSB0";

    char *path = getenv("MICROPYBTUART");
    if (path != NULL) {
        strcpy(uart_device_name, path);
    }
    DEBUG_printf("mp_bluetooth_hci_uart_init: Using HCI UART: %s\n", uart_device_name);

    int flags = O_RDWR | O_NOCTTY | O_NONBLOCK;
    uart_fd = open(uart_device_name, flags);
    if (uart_fd == -1) {
        printf("mp_bluetooth_hci_uart_init: Unable to open port %s\n", uart_device_name);
        return -1;
    }

    if (configure_uart()) {
        return -1;
    }

    // Create a thread to run the polling loop.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&hci_poll_thread_id, &attr, &hci_poll_thread, NULL);

    return 0;
}

int mp_bluetooth_hci_uart_deinit(void) {
    DEBUG_printf("mp_bluetooth_hci_uart_deinit\n");

    if (uart_fd == -1) {
        return 0;
    }

    // Wait for the poll loop to terminate when the state is set to OFF.
    pthread_join(hci_poll_thread_id, NULL);

    // Close the UART.
    close(uart_fd);
    uart_fd = -1;

    return 0;
}

int mp_bluetooth_hci_uart_set_baudrate(uint32_t baudrate) {
    (void)baudrate;
    DEBUG_printf("mp_bluetooth_hci_uart_set_baudrate\n");
    return 0;
}

int mp_bluetooth_hci_uart_readchar(void) {
    // DEBUG_printf("mp_bluetooth_hci_uart_readchar\n");

    if (uart_fd == -1) {
        return -1;
    }

    uint8_t c;
    ssize_t bytes_read = read(uart_fd, &c, 1);

    if (bytes_read == 1) {
        #if DEBUG_HCI_DUMP
        printf("[% 8ld] RX: %02x\n", mp_hal_ticks_ms(), c);
        #endif
        return c;
    } else {
        return -1;
    }
}

int mp_bluetooth_hci_uart_write(const uint8_t *buf, size_t len) {
    // DEBUG_printf("mp_bluetooth_hci_uart_write\n");

    if (uart_fd == -1) {
        return 0;
    }

    #if DEBUG_HCI_DUMP
    printf("[% 8ld] TX: %02x", mp_hal_ticks_ms(), buf[0]);
    for (size_t i = 1; i < len; ++i) {
        printf(":%02x", buf[i]);
    }
    printf("\n");
    #endif

    return write(uart_fd, buf, len);
}

// No-op implementations of HCI controller interface.
int mp_bluetooth_hci_controller_init(void) {
    return 0;
}

int mp_bluetooth_hci_controller_deinit(void) {
    return 0;
}

int mp_bluetooth_hci_controller_sleep_maybe(void) {
    return 0;
}

bool mp_bluetooth_hci_controller_woken(void) {
    return true;
}

int mp_bluetooth_hci_controller_wakeup(void) {
    return 0;
}

#endif // MICROPY_PY_BLUETOOTH && (MICROPY_BLUETOOTH_NIMBLE || (MICROPY_BLUETOOTH_BTSTACK && MICROPY_BLUETOOTH_BTSTACK_H4))
