#include "robojoy_session.h"

#include "ble_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Linux counterpart of RoboDriveCommClient_Update. The command period matches
 * ROBODRIVE_COMM_COMMAND_PERIOD_US in the firmware client. A missing response
 * reconnects so a dead link does not sit in expect_response forever.
 */

#define ROBOJOY_COMMAND_PERIOD_US 50000
#define ROBOJOY_RESPONSE_TIMEOUT_US 1000000

_Static_assert(sizeof(struct robodrive_header) == 12, "robodrive_header size");
_Static_assert(sizeof(struct robodrive_joy_cmd) == 20, "robodrive_joy_cmd size");
_Static_assert(sizeof(struct robodrive_joy_resp) == 20, "robodrive_joy_resp size");
_Static_assert(sizeof(struct robodrive_diff_drive_config_cmd_and_resp) == 20, "robodrive_diff_drive_config size");

struct robojoy_session
{
    ble_client *ble;
    char target[128];
    char error[256];
    int started;

    struct robodrive_joy_resp rsp;
    struct robodrive_diff_drive_config_cmd_and_resp config_rsp;
    uint8_t seqnum;
    bool expect_response;
    bool waiting_config;
    bool config_ready;
    bool config_failed;
    bool response_valid;
    bool protocol_fault;
    uint64_t last_command_us;
    uint32_t latency_ms;
    uint32_t valid_responses;
};

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void session_on_notify(const uint8_t *data, unsigned len, void *user)
{
    robojoy_session *session = user;
    struct robodrive_joy_resp rsp;

    if (!session->expect_response)
    {
        fprintf(stderr, "robojoy: unexpected notify, %u bytes\n", len);
        return;
    }

    if (session->waiting_config)
    {
        struct robodrive_diff_drive_config_cmd_and_resp cfg;
        struct robodrive_invalid_resp invalid;

        if (len == sizeof invalid)
        {
            memcpy(&invalid, data, sizeof invalid);
            if (robodrive_check_header(&invalid.hdr) &&
                invalid.hdr.msg_type == ROBODRIVE_MSG_INVALID &&
                invalid.hdr.seqnum == session->seqnum)
            {
                invalid.error_msg[sizeof invalid.error_msg - 1] = '\0';
                session->expect_response = false;
                session->waiting_config = false;
                session->config_failed = true;
                snprintf(session->error, sizeof session->error, "config rejected: %s",
                         invalid.error_msg[0] ? invalid.error_msg : "invalid");
                return;
            }
        }

        if (len != sizeof cfg)
        {
            fprintf(stderr, "robojoy: bad config response length %u\n", len);
            session->protocol_fault = true;
            session->expect_response = false;
            session->waiting_config = false;
            session->config_failed = true;
            snprintf(session->error, sizeof session->error, "bad config response length %u", len);
            return;
        }

        memcpy(&cfg, data, sizeof cfg);
        if (!robodrive_check_header(&cfg.hdr) ||
            cfg.hdr.msg_type != ROBODRIVE_MSG_DIFF_DRIVE_CONFIG ||
            cfg.hdr.seqnum != session->seqnum)
        {
            fprintf(stderr, "robojoy: bad config response type %u seq %u\n",
                    (unsigned)cfg.hdr.msg_type, (unsigned)cfg.hdr.seqnum);
            snprintf(session->error, sizeof session->error, "bad config response");
            session->protocol_fault = true;
            session->expect_response = false;
            session->waiting_config = false;
            session->config_failed = true;
            return;
        }

        session->config_rsp = cfg;
        session->config_ready = true;
        session->expect_response = false;
        session->waiting_config = false;
        session->latency_ms = (uint32_t)((monotonic_us() - session->last_command_us) / 1000ull);
        return;
    }

    if (len != sizeof rsp)
    {
        fprintf(stderr, "robojoy: bad response length %u\n", len);
        session->protocol_fault = true;
        return;
    }

    memcpy(&rsp, data, sizeof rsp);
    if (!robodrive_check_header(&rsp.hdr))
    {
        fprintf(stderr, "robojoy: bad response header magic=0x%02x fw=0x%02x\n",
                rsp.hdr.magic, rsp.hdr.fw_version);
        session->protocol_fault = true;
        return;
    }

    if (rsp.hdr.msg_type != ROBODRIVE_MSG_COMMAND)
    {
        fprintf(stderr, "robojoy: unexpected msg_type %u\n", (unsigned)rsp.hdr.msg_type);
        session->protocol_fault = true;
        return;
    }

    if (rsp.hdr.seqnum != session->seqnum)
    {
        fprintf(stderr, "robojoy: bad seq got %u expected %u\n",
                (unsigned)rsp.hdr.seqnum, (unsigned)session->seqnum);
        session->protocol_fault = true;
        return;
    }

    session->rsp = rsp;
    session->response_valid = true;
    session->expect_response = false;
    session->latency_ms = (uint32_t)((monotonic_us() - session->last_command_us) / 1000ull);
    session->valid_responses++;
}

static void copy_ble_error(robojoy_session *session)
{
    const char *error = ble_client_last_error(session->ble);
    if (error && error[0])
        snprintf(session->error, sizeof session->error, "%s", error);
}

static void sleep_ms(int timeout_ms)
{
    struct timespec ts;
    if (timeout_ms <= 0)
        return;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int session_drive(robojoy_session *session, struct robodrive_joy_cmd *cmd)
{
    uint64_t now = monotonic_us();

    if (session->protocol_fault)
    {
        session->protocol_fault = false;
        session->expect_response = false;
        ble_client_reconnect(session->ble);
    }
    else if (session->expect_response &&
             now - session->last_command_us > ROBOJOY_RESPONSE_TIMEOUT_US)
    {
        fprintf(stderr, "robojoy: response timeout\n");
        session->expect_response = false;
        ble_client_reconnect(session->ble);
    }

    if (!ble_client_ready(session->ble))
    {
        session->expect_response = false;
        session->last_command_us = 0;
        return 0;
    }

    if (session->expect_response)
        return 0;

    if (session->last_command_us != 0 &&
        now - session->last_command_us < ROBOJOY_COMMAND_PERIOD_US)
    {
        return 0;
    }

    session->seqnum++;
    cmd->key = session->rsp.key;
    cmd->hdr = default_robodrive_hdr;
    cmd->hdr.msg_type = ROBODRIVE_MSG_COMMAND;
    cmd->hdr.seqnum = session->seqnum;
    session->expect_response = true;
    session->last_command_us = monotonic_us();

    if (ble_client_write(session->ble, (const uint8_t *)cmd, sizeof *cmd) != 0 &&
        session->expect_response)
    {
        session->expect_response = false;
        copy_ble_error(session);
        ble_client_reconnect(session->ble);
    }

    return 0;
}

static int session_config_exchange(robojoy_session *session,
                                   enum robodrive_config_operation operation,
                                   const struct robodrive_diff_drive_config *config,
                                   struct robodrive_diff_drive_config *out,
                                   int timeout_ms)
{
    struct robodrive_diff_drive_config_cmd_and_resp req;
    uint64_t deadline;
    uint64_t now;

    if (!session || !session->started)
        return -1;
    if (!ble_client_ready(session->ble))
    {
        snprintf(session->error, sizeof session->error, "link is not ready");
        return -1;
    }
    if (session->waiting_config)
    {
        snprintf(session->error, sizeof session->error, "config request already in progress");
        return -1;
    }

    if (timeout_ms < 0)
        timeout_ms = 0;
    deadline = monotonic_us() + (uint64_t)timeout_ms * 1000ull;

    while (session->expect_response)
    {
        now = monotonic_us();
        if (now >= deadline || now - session->last_command_us > ROBOJOY_RESPONSE_TIMEOUT_US)
        {
            session->expect_response = false;
            snprintf(session->error, sizeof session->error, "timed out waiting to send config");
            return -1;
        }
        ble_client_poll(session->ble, 50);
    }

    memset(&req, 0, sizeof req);
    session->seqnum++;
    req.hdr = default_robodrive_hdr;
    req.hdr.msg_type = ROBODRIVE_MSG_DIFF_DRIVE_CONFIG;
    req.hdr.seqnum = session->seqnum;
    req.operation = operation;
    if (config)
        req.config = *config;

    session->config_ready = false;
    session->config_failed = false;
    session->error[0] = '\0';
    session->waiting_config = true;
    session->expect_response = true;
    session->last_command_us = monotonic_us();

    if (ble_client_write(session->ble, (const uint8_t *)&req, sizeof req) != 0)
    {
        session->expect_response = false;
        session->waiting_config = false;
        copy_ble_error(session);
        if (session->error[0] == '\0')
            snprintf(session->error, sizeof session->error, "config write failed");
        return -1;
    }

    while (!session->config_ready && !session->config_failed)
    {
        now = monotonic_us();
        if (session->protocol_fault || now >= deadline ||
            now - session->last_command_us > ROBOJOY_RESPONSE_TIMEOUT_US)
        {
            session->expect_response = false;
            session->waiting_config = false;
            if (session->error[0] == '\0')
                snprintf(session->error, sizeof session->error, "config response timeout");
            return -1;
        }
        ble_client_poll(session->ble, 50);
    }

    if (session->config_failed || !session->config_ready)
        return -1;
    if (session->config_rsp.operation != operation)
    {
        snprintf(session->error, sizeof session->error, "config operation mismatch");
        return -1;
    }
    if (out)
        *out = session->config_rsp.config;
    return 0;
}

static int poll_wait_ms(robojoy_session *session, int timeout_ms)
{
    uint64_t now;
    uint64_t elapsed;

    if (timeout_ms < 0)
        timeout_ms = 0;
    if (!ble_client_ready(session->ble) || session->expect_response)
        return timeout_ms;

    now = monotonic_us();
    if (session->last_command_us == 0)
        return 0;

    elapsed = now - session->last_command_us;
    if (elapsed >= ROBOJOY_COMMAND_PERIOD_US)
        return 0;

    {
        int remain_ms = (int)((ROBOJOY_COMMAND_PERIOD_US - elapsed) / 1000ull);
        if (remain_ms < timeout_ms)
            return remain_ms;
    }
    return timeout_ms;
}

robojoy_session *robojoy_session_create(const robojoy_session_config *cfg)
{
    robojoy_session *session;
    ble_client_config ble_cfg;
    const char *name = NULL;
    const char *address = NULL;
    const char *target;

    if (!cfg)
        return NULL;

    session = calloc(1, sizeof(*session));
    if (!session)
        return NULL;

    address = cfg->address;
    name = cfg->device_name;
    if (address && address[0] == '\0')
        address = NULL;
    if (name && name[0] == '\0')
        name = NULL;
    if (!address && !name)
        name = "ROBODRIVE_2025";

    target = address ? address : name;
    snprintf(session->target, sizeof session->target, "%s", target);

    ble_cfg.adapter = cfg->adapter;
    ble_cfg.device_name = address ? NULL : name;
    ble_cfg.address = address;
    session->ble = ble_client_create(&ble_cfg);
    if (!session->ble)
    {
        free(session);
        return NULL;
    }

    ble_client_set_notify_handler(session->ble, session_on_notify, session);
    return session;
}

void robojoy_session_destroy(robojoy_session *session)
{
    if (!session)
        return;
    ble_client_destroy(session->ble);
    free(session);
}

int robojoy_session_connect(robojoy_session *session, int timeout_ms)
{
    uint64_t deadline;

    if (!session)
        return -1;

    session->error[0] = '\0';
    if (!session->started)
    {
        if (ble_client_start(session->ble) != 0)
        {
            copy_ble_error(session);
            if (session->error[0] == '\0')
                snprintf(session->error, sizeof session->error, "failed to start bluetooth");
            sleep_ms(timeout_ms);
            return -1;
        }
        session->started = 1;
    }

    if (timeout_ms < 0)
        timeout_ms = 0;
    deadline = monotonic_us() + (uint64_t)timeout_ms * 1000ull;
    while (monotonic_us() < deadline)
    {
        ble_client_poll(session->ble, 100);
        if (ble_client_ready(session->ble))
        {
            session->error[0] = '\0';
            return 0;
        }
    }

    copy_ble_error(session);
    if (session->error[0] == '\0')
    {
        snprintf(session->error, sizeof session->error, "timed out waiting for %s", session->target);
    }
    return -1;
}

int robojoy_session_step(robojoy_session *session, struct robodrive_joy_cmd *cmd, int timeout_ms)
{
    if (!session || !cmd)
        return -1;
    if (!session->started)
    {
        snprintf(session->error, sizeof session->error, "session is not started");
        return -1;
    }

    ble_client_poll(session->ble, poll_wait_ms(session, timeout_ms));
    return session_drive(session, cmd);
}

int robojoy_session_config_read(robojoy_session *session, struct robodrive_diff_drive_config *config, int timeout_ms)
{
    if (!config)
        return -1;
    return session_config_exchange(session, ROBODRIVE_CONFIG_OPERATION_READ, NULL, config, timeout_ms);
}

int robojoy_session_config_set(robojoy_session *session, const struct robodrive_diff_drive_config *config, int timeout_ms)
{
    if (!config)
        return -1;
    return session_config_exchange(session, ROBODRIVE_CONFIG_OPERATION_SET, config, NULL, timeout_ms);
}

int robojoy_session_config_save(robojoy_session *session, const struct robodrive_diff_drive_config *config, int timeout_ms)
{
    if (!config)
        return -1;
    return session_config_exchange(session, ROBODRIVE_CONFIG_OPERATION_SAVE, config, NULL, timeout_ms);
}

bool robojoy_session_ready(const robojoy_session *session)
{
    return session && ble_client_ready(session->ble);
}

const struct robodrive_joy_resp *robojoy_session_response(const robojoy_session *session)
{
    if (!session || !session->response_valid)
        return NULL;
    return &session->rsp;
}

uint32_t robojoy_session_latency_ms(const robojoy_session *session)
{
    if (!session)
        return 0;
    return session->latency_ms;
}

uint32_t robojoy_session_take_response_count(robojoy_session *session)
{
    uint32_t count;
    if (!session)
        return 0;
    count = session->valid_responses;
    session->valid_responses = 0;
    return count;
}

int robojoy_session_rssi(const robojoy_session *session)
{
    if (!session)
        return 0;
    return ble_client_rssi(session->ble);
}

const char *robojoy_session_last_error(const robojoy_session *session)
{
    if (!session)
        return "";
    return session->error;
}

const char *robojoy_session_target(const robojoy_session *session)
{
    if (!session)
        return "";
    return session->target;
}
