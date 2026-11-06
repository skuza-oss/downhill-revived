/* Visible settings tabs, in menu order; hosts may hide every row of a tab. */
#ifndef DHD_SETTINGS_TABS_H
#define DHD_SETTINGS_TABS_H
static int settings_tabs_choose(int *out, int tabs, const int *row_tab, const int *visible,
                                int rows, int controls_tab, int controls)
{
    int count = 0;
    for (int t = 0; t < tabs; t++) {
        int any = t == controls_tab && controls;
        for (int i = 0; i < rows; i++) any |= visible[i] && row_tab[i] == t;
        if (any) out[count++] = t;
    }
    return count;
}
static int settings_tab_step(int tab, int step, int count)
{
    return count > 0 ? (tab + step + count) % count : 0;
}
#endif
