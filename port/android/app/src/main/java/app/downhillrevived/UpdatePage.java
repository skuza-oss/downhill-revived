// Downhill Revived on Android: which page the update notice may open (finding 143). Apart from
// DhdActivity so a plain JVM test runs it (port/android/test/UpdatePageTest.java). Nothing here
// uses Android.
package app.downhillrevived;

import java.util.Locale;

final class UpdatePage {
    /** This repository: the only pages the notice opens. */
    static final String REPOSITORY = "https://github.com/skuza-oss/downhill-revived/";
    /** The fixed releases page, for a page that isn't one of the repository's. */
    static final String RELEASES = REPOSITORY + "releases/latest";

    private UpdatePage() {}

    /** The page itself when it is the repository's, else the releases page. A page is the
     *  repository's when it starts with its address and nothing after that can take a browser out of
     *  it: no dot segment, plain or percent-encoded, no backslash (read as a slash), no space or
     *  quote. The same rule as port/net/update_check.c, the launcher's update.rs and the server's
     *  latest.ts. */
    static String safe(String page) {
        if (page == null || !page.startsWith(REPOSITORY)) return RELEASES;
        String rest = page.substring(REPOSITORY.length()).toLowerCase(Locale.ROOT);
        if (rest.contains("..") || rest.contains("%2e") || rest.contains("\\")) return RELEASES;
        for (char c : rest.toCharArray())
            if (c <= ' ' || c == '"' || c == '\'' || c == '<' || c == '>') return RELEASES;
        return page;
    }
}
