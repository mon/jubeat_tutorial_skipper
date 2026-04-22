#pragma once

#include <stdarg.h>

// Ordinals in avs2-core.def are pinned to the AVS 1700 ABI (see
// bemanitools' import_32_1700_avs.def); newer AVS versions renumber them.
extern "C" {
    void log_body_fatal  (const char *module, const char *fmt, ...);
    void log_body_warning(const char *module, const char *fmt, ...);
    void log_body_info   (const char *module, const char *fmt, ...);
    void log_body_misc   (const char *module, const char *fmt, ...);
    void vlog_body(int level, const char *module, const char *fmt, va_list args);
}
