#pragma once
// Host stand-in for the generated sdkconfig.h, holding only what
// firmware/common/location/src/os_h3_alloc.cpp reads. Both board
// sdkconfig.defaults set this prefix, and tools/test_h3_polygon.py builds the
// library with the same one.
#define CONFIG_EMBEDDED_H3_LOCATION_ALLOC_PREFIX "os_h3_"
