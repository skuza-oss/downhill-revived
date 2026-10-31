/* The release this build is: the one place its version is written (finding 054).

   - The online hello sends it as clientVersion; the server refuses releases older than its
     MIN_VERSION (online/server/wrangler.jsonc).
   - The launcher (port/launcher, build.rs reads it) compares it with the newest release's latest.json.
   - port/tools/package_release.py names the zip after it and writes latest.json for it.

   Bump it here, and only here, for each release: "MAJOR.MINOR.PATCH". */
#ifndef DHD_VERSION_H
#define DHD_VERSION_H

#define DHD_VERSION "0.7.5"

#endif
