#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct ble_client ble_client;

typedef void (*ble_notify_fn)(const uint8_t *data, unsigned len, void *user);

typedef struct ble_client_config
{
    const char *adapter;     /* NULL selects hci0 */
    const char *device_name; /* exact advertising name; ignored when address is set */
    const char *address;     /* "AA:BB:CC:DD:EE:FF", or NULL to select by name */
} ble_client_config;

/* Single-threaded. The client owns this thread's GLib main context until destroy. */
ble_client *ble_client_create(const ble_client_config *cfg);
void ble_client_destroy(ble_client *client);

/* Open BlueZ and start scanning. Safe to call again after a failed start. */
int ble_client_start(ble_client *client);

/* Dispatch D-Bus events for up to timeout_ms. Returns 0. */
int ble_client_poll(ble_client *client, int timeout_ms);

bool ble_client_ready(const ble_client *client);

/* Write-without-response of a raw command. Returns 0 on success. */
int ble_client_write(ble_client *client, const uint8_t *data, unsigned len);

void ble_client_set_notify_handler(ble_client *client, ble_notify_fn fn, void *user);

/* Drop the link and connect again. No effect while already scanning. */
void ble_client_reconnect(ble_client *client);

/* Last RSSI sample in dBm, or 0 when BlueZ has not reported one. */
int ble_client_rssi(const ble_client *client);

const char *ble_client_last_error(const ble_client *client);
