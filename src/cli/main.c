#include "robojoy_session.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void format_rsp_flags(uint8_t flags, char *buf, size_t len)
{
    static const struct
    {
        uint8_t bit;
        const char *name;
    } table[] = {
        { 0x01, "enabled" },
        { 0x02, "fault" },
        { 0x04, "low_batt" },
        { 0x08, "timeout" },
        { 0x10, "bad_key" },
    };
    size_t used = 0;
    size_t i;

    if (len == 0)
        return;
    buf[0] = '\0';
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
    {
        int wrote;
        if ((flags & table[i].bit) == 0)
            continue;
        wrote = snprintf(buf + used, len - used, "%s%s", used ? "," : "", table[i].name);
        if (wrote < 0 || (size_t)wrote >= len - used)
            return;
        used += (size_t)wrote;
    }
    if (used == 0)
        snprintf(buf, len, "none");
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Send zero joystick commands to a RoboDrive and print feedback once a second.\n"
            "flags stay 0, so the drive enable bit is clear.\n"
            "\n"
            "  -n, --name NAME     Advertising name (default ROBODRIVE_2025)\n"
            "  -a, --address ADDR  Bluetooth address AA:BB:CC:DD:EE:FF\n"
            "  -A, --adapter HCI   Adapter name (default hci0)\n"
            "  -r, --read-config   Read diff-drive config, print it, and exit\n"
            "  -h, --help\n"
            "\n"
            "Known names: BOO_FRIENDS, ROBODRIVE_2025, AUSTIN_OCT2026, BOX_BOX_BOX\n",
            argv0);
}

static void print_diff_drive_config(const struct robodrive_diff_drive_config *config)
{
    printf("flags=0x%02x", (unsigned)config->flags);
    if (config->flags & ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_1)
        printf(" reverse_motor_1");
    if (config->flags & ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_2)
        printf(" reverse_motor_2");
    if (config->flags & ROBODRIVE_DIFF_DRIVE_FLAG_SWAP_MOTORS)
        printf(" swap");
    if (config->flags == 0)
        printf(" none");
    printf(" full_scale=%u turn_scale=%u\n",
           (unsigned)config->full_scale, (unsigned)config->turn_scale);
    fflush(stdout);
}

static int read_config_and_exit(robojoy_session *session)
{
    struct robodrive_diff_drive_config config;
    uint64_t deadline = monotonic_us() + 20000000ull;

    while (!g_stop && !robojoy_session_ready(session) && monotonic_us() < deadline)
        robojoy_session_connect(session, 200);

    if (!robojoy_session_ready(session))
    {
        const char *error = robojoy_session_last_error(session);
        fprintf(stderr, "robojoy: %s\n", error[0] ? error : "timed out connecting");
        return 1;
    }

    if (robojoy_session_config_read(session, &config, 2000) != 0)
    {
        const char *error = robojoy_session_last_error(session);
        fprintf(stderr, "robojoy: %s\n", error[0] ? error : "config read failed");
        return 1;
    }

    print_diff_drive_config(&config);
    return 0;
}

static void print_status(robojoy_session *session)
{
    const struct robodrive_joy_resp *rsp;
    uint32_t rx = robojoy_session_take_response_count(session);
    char flags[64];
    int rssi;

    if (!robojoy_session_ready(session))
    {
        const char *error = robojoy_session_last_error(session);
        if (error[0] && strstr(error, "timed out waiting") == NULL)
            printf("%s\n", error);
        else
            printf("scanning for %s\n", robojoy_session_target(session));
        fflush(stdout);
        return;
    }

    rsp = robojoy_session_response(session);
    if (!rsp)
    {
        printf("link ready, waiting for response\n");
        fflush(stdout);
        return;
    }

    format_rsp_flags(rsp->flags, flags, sizeof flags);
    rssi = robojoy_session_rssi(session);
    printf("fw=%u.%u seq=%u key=%u flags=0x%02x %s batt=%u%% cpu_s=%u motor_s=%u rtt_ms=%u rx=%u",
           (unsigned)get_fw_version_major(rsp->hdr.fw_version),
           (unsigned)get_fw_version_minor(rsp->hdr.fw_version),
           (unsigned)rsp->hdr.seqnum,
           (unsigned)rsp->key,
           (unsigned)rsp->flags,
           flags,
           (unsigned)rsp->batt_percent,
           (unsigned)rsp->cpu_uptime,
           (unsigned)rsp->motor_uptime,
           (unsigned)robojoy_session_latency_ms(session),
           (unsigned)rx);
    if (rssi != 0)
        printf(" rssi=%d", rssi);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "name", required_argument, NULL, 'n' },
        { "address", required_argument, NULL, 'a' },
        { "adapter", required_argument, NULL, 'A' },
        { "read-config", no_argument, NULL, 'r' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    const char *name = NULL;
    const char *address = NULL;
    const char *adapter = NULL;
    robojoy_session_config cfg;
    robojoy_session *session;
    uint64_t next_print;
    int opt;

    int read_config = 0;

    while ((opt = getopt_long(argc, argv, "n:a:A:rh", options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'n':
            name = optarg;
            break;
        case 'a':
            address = optarg;
            break;
        case 'A':
            adapter = optarg;
            break;
        case 'r':
            read_config = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind != argc)
    {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&cfg, 0, sizeof cfg);
    cfg.adapter = adapter;
    cfg.device_name = name ? name : "ROBODRIVE_2025";
    cfg.address = address;

    session = robojoy_session_create(&cfg);
    if (!session)
    {
        fprintf(stderr, "robojoy: out of memory\n");
        return 1;
    }

    if (read_config)
    {
        int rc = read_config_and_exit(session);
        robojoy_session_destroy(session);
        return rc;
    }

    fprintf(stderr, "robojoy: sending zero sticks, drive disabled, target %s\n",
            robojoy_session_target(session));

    next_print = 0;
    while (!g_stop)
    {
        if (!robojoy_session_ready(session))
            robojoy_session_connect(session, 200);
        else
        {
            struct robodrive_joy_cmd cmd;
            memset(&cmd, 0, sizeof cmd);
            robojoy_session_step(session, &cmd, 50);
        }

        if (monotonic_us() >= next_print)
        {
            print_status(session);
            next_print = monotonic_us() + 1000000ull;
        }
    }

    robojoy_session_destroy(session);
    return 0;
}
