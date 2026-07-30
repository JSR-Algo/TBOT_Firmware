#pragma once

// Match esp_jpeg's non-ROM Kconfig defaults; target-specific values come from the test command.
#define CONFIG_JD_SZBUF 512
#define CONFIG_JD_FORMAT 0
#define CONFIG_JD_USE_SCALE 1
#define CONFIG_JD_TBLCLIP 1
#define CONFIG_JD_FASTDECODE 1
