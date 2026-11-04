#include "robodrive_comm.h"

struct robodrive_header default_robodrive_hdr = {
    .magic = ROBODRIVE_MAGIC,
    .fw_version = (FW_VERSION_MAJOR << 4) | FW_VERSION_MINOR,
    .msg_type = ROBODRIVE_MSG_INVALID
};

uint8_t get_fw_version_major(uint8_t fw_version)
{
    return (fw_version >> 4) & 0xF;
}

uint8_t get_fw_version_minor(uint8_t fw_version)
{
    return fw_version & 0xF;
}

bool robodrive_check_header(struct robodrive_header* hdr)
{
    return ((hdr->magic == ROBODRIVE_MAGIC) && (get_fw_version_major(hdr->fw_version) == FW_VERSION_MAJOR));
}