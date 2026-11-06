#include "settings_tabs.h"
#include <assert.h>
int main(void)
{
    int rows[] = {1, 0, 2, 3, 4}, visible[] = {1, 1, 1, 1, 1}, out[5];
    assert(settings_tabs_choose(out, 5, rows, visible, 5, 2, 0) == 5);
    for (int i = 0; i < 5; i++) assert(out[i] == i);
    visible[1] = visible[2] = visible[3] = 0;
    assert(settings_tabs_choose(out, 5, rows, visible, 5, 2, 0) == 2);
    assert(out[0] == 1 && out[1] == 4);
    assert(settings_tabs_choose(out, 5, rows, visible, 5, 2, 1) == 3);
    assert(out[0] == 1 && out[1] == 2 && out[2] == 4);
    for (int i = 0; i < 5; i++) visible[i] = 0;
    assert(settings_tabs_choose(out, 5, rows, visible, 5, 2, 0) == 0);
    assert(settings_tab_step(0, -1, 3) == 2);
    assert(settings_tab_step(2, 1, 3) == 0);
    assert(settings_tab_step(0, 1, 1) == 0);
    assert(settings_tab_step(0, 1, 0) == 0);
    return 0;
}
