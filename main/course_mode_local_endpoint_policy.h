#ifndef COURSE_MODE_LOCAL_ENDPOINT_POLICY_H_
#define COURSE_MODE_LOCAL_ENDPOINT_POLICY_H_

#include <string_view>

bool IsValidCourseModeOtaUrl(std::string_view url);
bool IsValidCourseModeWebsocketUrl(std::string_view url);

#endif  // COURSE_MODE_LOCAL_ENDPOINT_POLICY_H_
