/* What the module's C needs from its compiler and C library, in one place (docs/findings/081-portable-core.md,
   phase 2 and 3). The module is the 32-bit Windows build today and wasm32 later. Each macro here
   expands to exactly the code the Windows build had before, so the i686 build is unchanged. */
#ifndef DHD_PORTABLE_H
#define DHD_PORTABLE_H

#include <string.h>

/* Case-insensitive string compares and strdup: the MSVC CRT names on Windows, POSIX elsewhere. */
#if defined(_WIN32)
#define dhd_stricmp(a, b) _stricmp((a), (b))
#define dhd_strnicmp(a, b, n) _strnicmp((a), (b), (n))
#define dhd_strdup(s) _strdup(s)
#else
#include <strings.h>
#define dhd_stricmp(a, b) strcasecmp((a), (b))
#define dhd_strnicmp(a, b, n) strncasecmp((a), (b), (n))
#define dhd_strdup(s) strdup(s)
#endif

/* A function that must not be inlined (the Windows spelling, or GCC's attribute). */
#if defined(_WIN32)
#define PS2_NOINLINE __declspec(noinline)
#else
#define PS2_NOINLINE __attribute__((noinline))
#endif

/* The return address of the calling function, for diagnostics. wasm has no such builtin. */
#if defined(__wasm__)
#define PS2_CALLER_ADDRESS() ((const void *)0)
#else
#define PS2_CALLER_ADDRESS() __builtin_return_address(0)
#endif

/* An optimizer barrier on an SSE value (the VU's operands must not be folded, ps2 vu0_interp.c
   operand4): an empty asm on x86, and a volatile round trip where there is no inline asm. */
#if defined(__i386__) || defined(__x86_64__)
#define PS2_OPAQUE_M128(v) __asm__("" : "+x"(v))
#elif defined(__aarch64__)
/* the value stays in its NEON register (the volatile round trip below cost a 16-byte store and
   reload per VU operand on the phone, finding 091 AND3) */
#define PS2_OPAQUE_M128(v) __asm__("" : "+w"(v))
#else
#define PS2_OPAQUE_M128(v) do { volatile __typeof__(v) ps2_opaque_ = (v); (v) = ps2_opaque_; } while (0)
#endif

#ifndef PS2_NO_AUTO_INIT
/* The build zero-fills every local (-ftrivial-auto-var-init=zero: the game reads some before
   writing them, finding 081). A port local every read of which follows a write can skip that
   fill where it costs (finding 148). */
#if defined(__has_attribute)
#if __has_attribute(uninitialized)
#define PS2_NO_AUTO_INIT __attribute__((uninitialized))
#endif
#endif
#ifndef PS2_NO_AUTO_INIT
#define PS2_NO_AUTO_INIT
#endif
#endif

#endif
