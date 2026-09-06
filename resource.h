#pragma once

// Classic screensavers took their Control Panel name from string 1. Modern
// Windows reads the version resource's FileDescription instead, so Storm.rc
// supplies both.
#define IDS_DESCRIPTION 1

#define STORM_VERSION_MAJOR 0
#define STORM_VERSION_MINOR 1
#define STORM_VERSION_PATCH 0
#define STORM_VERSION_TEXT  L"0.1.0"
#define STORM_VERSION_ASCII  "0.1.0"
