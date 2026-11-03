/* Standalone: clang port/hle/test_online_pickup.c -o <scratch>/test-pickup */
#include "online_pickup.h"
#include <assert.h>
int main(void)
{
    uint8_t e[PICKUP_EVENT_SIZE];
    float xyz[3] = {123.5f, -4.0f, 88.0f}, other[3] = {123.5f, -4.0f, 89.0f};
    pickup_encode(e, 149, 9, 5, 25, xyz);
    assert(e[2] == 149 && e[3] == 9 && e[4] == 5 && e[5] == 25);
    assert(pickup_matches(e, sizeof e, 150, 5, 25, xyz));
    assert(!pickup_matches(e, sizeof e - 1, 150, 5, 25, xyz));
    assert(!pickup_matches(e, sizeof e, 149, 5, 25, xyz));
    assert(!pickup_matches(e, sizeof e, 150, 4, 25, xyz));
    assert(!pickup_matches(e, sizeof e, 150, 5, 24, xyz));
    assert(!pickup_matches(e, sizeof e, 150, 5, 25, other));
    e[2] = 150; assert(!pickup_matches(e, sizeof e, 151, 5, 25, xyz));
    e[2] = 149; e[3] = 10; assert(!pickup_matches(e, sizeof e, 150, 5, 25, xyz));
    e[3] = 9; e[0] = 1; assert(!pickup_matches(e, sizeof e, 150, 5, 25, xyz));
    e[0] = 2; e[1] = 8; assert(!pickup_matches(e, sizeof e, 150, 5, 25, xyz));
    return 0;
}
