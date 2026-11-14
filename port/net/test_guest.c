/* The guest boundary's base for the network test programs (build_tests.py): net.c carries
   the game's host_service_* wrappers, which read guest memory through it; the tests never call them. */
#include <stdint.h>

uintptr_t guest_base;
