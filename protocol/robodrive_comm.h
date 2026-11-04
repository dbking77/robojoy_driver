#pragma once

#include <stdint.h>
#include <stdbool.h>

#define FW_VERSION_MAJOR 2
#define FW_VERSION_MINOR 0
#define ROBODRIVE_MAGIC 0xDE
//#define ROBODRIVE_BLE_ADV_NAME "ROBODRIVE_2025"
//#define ROBODRIVE_BLE_ADV_NAME "BOO_FRIENDS"
#define ROBODRIVE_BLE_ADV_NAME "AUSTIN_OCT2026"

// Every exchange is command-response: the GATT client sends a packet,
// and the robodrive GATT server replies with the response for that packet.
// Request and response share the same msg_type.
// Values >= ROBODRIVE_MSG_RESERVED are reserved.
enum robodrive_msg_type
{
    ROBODRIVE_MSG_INVALID = 0,            // response when previous command was invalid/not supported
    ROBODRIVE_MSG_COMMAND = 1,            // robodrive_joy_cmd / robodrive_joy_resp
    ROBODRIVE_MSG_DIFF_DRIVE_CONFIG = 2,  // robodrive_diff_drive_config_cmd_and_resp
    // 3-255 reserved for future use
};

struct robodrive_header
{
    uint8_t magic;       // Magic number 0xDE

    // MSB nibble major, LSB nibble minor
    // Major number should match for FW to be compatible
    uint8_t fw_version;

    enum robodrive_msg_type msg_type;

    // Command counts up each msg,
    // Response seqnum should match command seqnum
    uint8_t seqnum;
};

enum robodrive_invalid_error_code
{
    ROBODRIVE_INVALID_ERROR_CODE_INVALID_COMMAND = 0,
    ROBODRIVE_INVALID_ERROR_CODE_INVALID_ARG = 1,

    // Failed to execute command, check error_msg for details
    ROBODRIVE_INVALID_ERROR_CODE_FAILED = 2,
};

struct robodrive_invalid_resp
{
    struct robodrive_header hdr;
    enum robodrive_invalid_error_code error_code;
    char error_msg[32];
};

extern struct robodrive_header default_robodrive_hdr;

struct robodrive_joy_cmd
{
    struct robodrive_header hdr;

    // Should match previous response key for drive to be enabled
    // prevents robodrive from operation getting out-of-sync with drive
    uint8_t key;

    // bit0 = enable drive / servo
    // bit1 = right jostick button : 1 = pressed
    // bit2 = left joystick button : 1 = pressed
    uint8_t flags;

    // Right joystick
    int8_t r_joy_fwd;    // -100 to 100  : positive forward, negative backward
    int8_t r_joy_right;  // -100 to 100  :  positive right, negative left

    // Left joystick
    int8_t l_joy_fwd;    // -100 to 100  : positive forward, negative backward
    int8_t l_joy_right;  // -100 to 100  :  positive right, negative left
};

struct robodrive_joy_resp
{
    struct robodrive_header hdr;

    // key that joystick shoud use to
    uint8_t key;

    // bit 0 enabled
    // bit 1 motor fault
    // bit 2 battery voltage low
    // bit 3 timeout
    // bit 4 bad key
    uint8_t flags;

    // Battery level in percentage 0 to 100
    uint8_t batt_percent;

    // Seconds since last reboot , saturates at 255
    // Used to detect brown-outs / unexpected resets
    uint8_t cpu_uptime;

    // Seconds that motor has been operating
    uint8_t motor_uptime;
};

// Operation for different configuration commands
enum robodrive_config_operation
{
    ROBODRIVE_CONFIG_OPERATION_READ = 0,  // read configuration
    ROBODRIVE_CONFIG_OPERATION_SET = 1, // set configuration (in ram temporarily)
    ROBODRIVE_CONFIG_OPERATION_SAVE = 2, // save configuration to flash
};


// Flags for robodrive_diff_drive_config_cmd_and_resp.flags
// bit 3-7 = reserved
#define ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_1 (1u << 0)
#define ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_2 (1u << 1)
#define ROBODRIVE_DIFF_DRIVE_FLAG_SWAP_MOTORS     (1u << 2)

struct robodrive_diff_drive_config
{
    // Response will contain the new flags on write, or current flags on read
    uint8_t flags;

    // 0 to 100. 100 applies the full joystick forward command to the wheels.
    uint8_t full_scale;

    // 0 to 100. 100 applies the full joystick turn command to the wheels.
    uint8_t turn_scale;
};

// For differential drive configuration, the command and response the same
struct robodrive_diff_drive_config_cmd_and_resp
{
    struct robodrive_header hdr;

    // Operation can write new configuration or just read current configuration
    // Response will just echo the operation
    enum robodrive_config_operation operation;

    struct robodrive_diff_drive_config config;
};



uint8_t get_fw_version_major(uint8_t fw_version);

uint8_t get_fw_version_minor(uint8_t fw_version);

bool robodrive_check_header(struct robodrive_header* hdr);