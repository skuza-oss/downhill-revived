// Downhill Revived on Android: the launcher's native calls (finding 225), in libmain.so
// (port/host/android_launcher.c): the shared code of the settings (dhd_settings), the disc checks
// (host.c, dhd_disc) and the update check (port/net/update_check.c), so the launcher keeps no copy of
// them in Java. Loading the library starts no game: SDL_main is DhdActivity's.
package app.downhillrevived;

final class Native {
    static {
        System.loadLibrary("main");
    }

    private Native() {}

    /** Every setting's default, choices and rule: the page's settings_schema (JSON). */
    static native String schema();
    /** Every setting's value in dhd.ini at ini, else its default (JSON object). */
    static native String values(String ini);
    /** The value dhd.ini at ini gives key, as written; null when it has none. */
    static native String read(String ini, String key);
    /** key = value saved in dhd.ini at ini, every other line kept. */
    static native boolean save(String ini, String key, String value);
    /** The rider name saved for what the player typed ("" none). */
    static native String riderName(String typed);
    /** null when the file at path is Downhill Domination (USA), else why not (host.c's check). */
    static native String discProblem(String path);
    /** The online check (dhd_disc_check, with iso-check.txt at cache): "code\nnote". */
    static native String onlineCheck(String iso, String cache);
    /** "version\npage" when a newer release has an Android build, "" when none does, null offline. */
    static native String update();
    /** This build's version. */
    static native String version();
}
