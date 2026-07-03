#pragma once

#include <string_view>

namespace Lang {
namespace Strings {
static constexpr const char* ERROR = "Lỗi";
static constexpr const char* PLEASE_WAIT = "Vui lòng đợi...";
}
namespace Sounds {
static constexpr std::string_view OGG_EXCLAMATION{"exclamation"};
static constexpr std::string_view OGG_POPUP{"popup"};
static constexpr std::string_view OGG_SUCCESS{"success"};
}
}
