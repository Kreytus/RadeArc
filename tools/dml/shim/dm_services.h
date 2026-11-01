// Userspace stand-in for the kernel's dm_services.h, just enough for Linux's DML (dc/dml) to compile as plain C.
// The DML code itself is unmodified (tools/dml/fetch_dml.sh). Logging and asserts compile to nothing.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ASSERT(x) ((void)0)
#define BREAK_TO_DEBUGGER() ((void)0)
#define dm_output_to_console(...) ((void)0)
#define dml_print(...) ((void)0)
#define DTRACE(...) ((void)0)
#define dml_assert(x) ((void)0)
#define noinline __attribute__((noinline))
#define noinline_for_stack noinline
#define DC_LOG_DML(...) ((void)0)
