/* The iOS app's two screens (finding 227): the launcher's page (launcher.m) and the game
   (main.m starts it in the launcher's place). GPL-3.0-or-later. */
#ifndef DHD_IOS_APP_H
#define DHD_IOS_APP_H

#ifdef __OBJC__
#import <UIKit/UIKit.h>
/* The launcher's page (the note after a run that closed unexpectedly: launcher_set_last_exit). */
UIViewController *launcher_view_controller(void);
#endif

/* The setting "launcher" is off and the disc is ready: the app starts the game at once. */
int launcher_should_start_game(void);
/* PLAY (main.m): the game takes the launcher's place. Main thread. */
void app_play(int edit_touch_layout, int checked);

#endif
