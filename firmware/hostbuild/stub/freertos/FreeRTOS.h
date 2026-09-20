#pragma once
#include <stdbool.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) (void)(x)
#define taskEXIT_CRITICAL(x) (void)(x)
