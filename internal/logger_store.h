#pragma once

#include <string_view>

#include "utils/logger.h"

namespace logger::detail {

bool storage_init(bool write_enabled);
bool storage_append(level_e level, std::string_view message);

}
