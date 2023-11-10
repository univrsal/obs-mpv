#pragma once
#include <stdbool.h>

bool wgl_init();

void wgl_deinit();

bool wgl_enter_context();

void wgl_exit_context();