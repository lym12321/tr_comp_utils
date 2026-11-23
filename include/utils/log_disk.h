#pragma once

#include "bsp/usb.h"

namespace logger {
    // Prepare once on maintenance boot before starting USB. The disk lives until reset.
    const bsp_usb_disk_t* prepare_log_disk();
}
