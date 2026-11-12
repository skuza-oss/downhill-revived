"""EXIT GAME (port/hle/exit_menu.c, finding 056): a fifth main menu row (func_001ED5B8) under
ONLINE that asks "Quit Downhill Revived?" in the game's popup and ends the game on Yes. Applied
after port/patches.py's ONLINE patch, on its text. Leaving the main menu by the game's rows still
puts its last row back to 2 (the ONLINE patch), so the gates' route is unchanged.

A build without online play (Android, finding 143) has no ONLINE row: port_main_rows() is 4 there
and port_main_choice() turns the shown fourth row into EXIT GAME's (online_menu.c); with online
(every other build) they are 5 and the row itself, as before.
"""

UNIT = 'unsorted/001EB1D8.cpp'


def add(PATCHES):
    PATCHES[UNIT] = PATCHES.get(UNIT, []) + [
        ('text', 'extern int port_main_label(int *table, int index);\n',
         'extern int port_main_label(int *table, int index);\n'
         'extern int port_exit_label(int *table, int index);\n'
         'extern void port_exit_menu(void);\n'
         'extern int port_disc_notice_due(void);\n'
         'extern void port_disc_notice(void);\n'
         'extern int port_main_rows(void);\n'
         'extern int port_main_choice(int shown);\n'),
        ('text', 'void func_001ED5B8__cED5B8(void) {\n    u8__cED5B8 disabled[4];\n    int index, choice;\n',
         'void func_001ED5B8__cED5B8(void) {\n    u8__cED5B8 disabled[5];\n    int index, choice;\n'
         # the disc notice (port/hle/disc_notice.c, finding 208): the main menu starts the disc
         # check, and when it says the image differs, shows its notice once, as EXIT GAME's popup
         '    if (port_disc_notice_due()) {\n'
         '        func_001F57D0__cED5B8(0x2329, 0);\n'
         '        port_disc_notice();\n'
         '        func_001F57D0__cED5B8(0x2329, 1);\n'
         '        return;\n'
         '    }\n'),
        ('text', 'func_001F7420__cED5B8(&D_004C6CA8__cED5B8, 10, 0, 0, 3, port_main_row, disabled, 4);',
         'func_001F7420__cED5B8(&D_004C6CA8__cED5B8, 10, 0, 0, port_main_rows() - 1, port_main_row, disabled,\n'
         '                      port_main_rows());'),
        ('text', '        choice = D_004C6CA8__cED5B8.x10;\n        port_main_row = 0;\n',
         '        choice = port_main_choice(D_004C6CA8__cED5B8.x10);\n        port_main_row = 0;\n'),
        ('re', (r'port_main_label\(D_0029E054__cED5B8, index\)', 3), 'port_exit_label(D_0029E054__cED5B8, index)'),
        ('text', '            port_main_row = port_online_menu() ? 0 : 3;\n'
                 '            func_001F57D0__cED5B8(0x2329, 1);\n        }\n',
         '            port_main_row = port_online_menu() ? 0 : 3;\n'
         '            func_001F57D0__cED5B8(0x2329, 1);\n'
         '        } else if (choice == 4) {\n'
         '            func_001F57D0__cED5B8(0x2329, 0);\n'
         '            port_exit_menu();\n'
         '            port_main_row = port_main_rows() - 1;\n'
         '            func_001F57D0__cED5B8(0x2329, 1);\n'
         '        }\n'),
    ]
