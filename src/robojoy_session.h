#pragma once

#include "robodrive_comm.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct robojoy_session robojoy_session;

typedef struct robojoy_session_config
{
    const char *adapter;     /* NULL selects hci0 */
    const char *device_name; /* NULL selects ROBODRIVE_2025; ignored when address is set */
    const char *address;     /* "AA:BB:CC:DD:EE:FF", or NULL to select by name */
} robojoy_session_config;

robojoy_session *robojoy_session_create(const robojoy_session_config *cfg);
void robojoy_session_destroy(robojoy_session *session);

/*
 * Scan, connect, and subscribe. Blocks until the link is up or timeout_ms
 * elapses. Returns 0 when ready, or -1 on timeout or setup failure.
 * A later call continues the same attempt.
 */
int robojoy_session_connect(robojoy_session *session, int timeout_ms);

/*
 * Pump the link. Overwrites hdr (including msg_type and seqnum) and key.
 * Leaves joystick axes and flags as the caller set them. Sends at most once
 * per 50 ms, and only after the previous response has arrived. timeout_ms is
 * the longest the call waits for bus events. Returns 0, or -1 if session or
 * cmd is NULL.
 */
int robojoy_session_step(robojoy_session *session, struct robodrive_joy_cmd *cmd, int timeout_ms);

/*
 * One config exchange. Pauses the joystick stream until the matching reply
 * arrives. Read ignores any previous config contents. Set updates drive RAM.
 * Save updates drive RAM and stores it in NVM. Returns 0, or -1 on failure.
 */
int robojoy_session_config_read(robojoy_session *session, struct robodrive_diff_drive_config *config, int timeout_ms);
int robojoy_session_config_set(robojoy_session *session, const struct robodrive_diff_drive_config *config, int timeout_ms);
int robojoy_session_config_save(robojoy_session *session, const struct robodrive_diff_drive_config *config, int timeout_ms);

bool robojoy_session_ready(const robojoy_session *session);

/* Last accepted response, or NULL before the first one. */
const struct robodrive_joy_resp *robojoy_session_response(const robojoy_session *session);

uint32_t robojoy_session_latency_ms(const robojoy_session *session);

/* Valid responses since the previous call, then resets the counter. */
uint32_t robojoy_session_take_response_count(robojoy_session *session);

/* Last RSSI sample in dBm, or 0 when unknown. */
int robojoy_session_rssi(const robojoy_session *session);

const char *robojoy_session_last_error(const robojoy_session *session);
const char *robojoy_session_target(const robojoy_session *session);
