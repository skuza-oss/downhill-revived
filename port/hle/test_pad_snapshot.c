#include "pad_snapshot.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    PadSender sender = {0};
    uint32_t pad[PAD_WORDS] = {0}, mirror[PAD_WORDS] = {0};
    uint8_t wire[731];
    int n = pad_snapshot_pack(&sender, pad, wire);
    assert(n == 731 && pad_snapshot_apply(mirror, wire, n));
    pad[2] = 123;
    /* Lose five consecutive changed packets; the sixth repairs the mirror. */
    for (int k = 0; k < 6; k++) {
        n = pad_snapshot_pack(&sender, pad, wire);
        assert(n == 6);
        if (k == 5) assert(pad_snapshot_apply(mirror, wire, n));
    }
    assert(mirror[2] == 123);
    assert(pad_snapshot_pack(&sender, pad, wire) == 1);
    /* Lose every repeated change: the periodic full refresh still repairs it. */
    pad[2] = 0; pad[170] = 999;
    for (int k = 0; k < 22; k++) pad_snapshot_pack(&sender, pad, wire);
    n = pad_snapshot_pack(&sender, pad, wire);
    assert(n == 731 && pad_snapshot_apply(mirror, wire, n));
    assert(mirror[2] == 0 && mirror[170] == 999);
    wire[1] = 255;
    assert(!pad_snapshot_apply(mirror, wire, n));
    assert(!pad_snapshot_apply(mirror, wire, n - 1));
    puts("pad snapshot loss recovery: OK");
    return 0;
}
