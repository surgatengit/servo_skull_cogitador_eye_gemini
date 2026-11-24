/* Host test stub: single-threaded tests need no real mutex. */
#pragma once
typedef int *SemaphoreHandle_t;
static int stub_mutex_storage;
#define xSemaphoreCreateMutex() (&stub_mutex_storage)
#define vSemaphoreDelete(m) ((void)(m))
#define xSemaphoreTake(m, t) ((void)(m), (void)(t), 1)
#define xSemaphoreGive(m) ((void)(m), 1)
