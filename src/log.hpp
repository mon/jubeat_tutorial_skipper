#pragma once

#include "avs2-core.hpp"  // IWYU pragma: keep  — used inside log_* macros

#ifndef LOG_MODULE
#define LOG_MODULE "tutorial_skip"
#endif

#define log_fatal(...)   log_body_fatal  (LOG_MODULE, __VA_ARGS__)
#define log_warning(...) log_body_warning(LOG_MODULE, __VA_ARGS__)
#define log_info(...)    log_body_info   (LOG_MODULE, __VA_ARGS__)
#define log_misc(...)    log_body_misc   (LOG_MODULE, __VA_ARGS__)
