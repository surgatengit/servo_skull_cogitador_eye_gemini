/* Host test stub: logs go to stdout only when VERBOSE is defined. */
#pragma once
#include <stdio.h>
#ifdef VERBOSE
#define ESP_LOGD(tag, fmt, ...) printf("D %s: " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGD(tag, fmt, ...) do { if (0) { printf("%s" fmt, tag, __VA_ARGS__); } } while (0)
#endif
