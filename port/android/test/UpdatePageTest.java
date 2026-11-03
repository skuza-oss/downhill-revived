// Tests of UpdatePage (the page the update notice opens, finding 143) on a plain JVM; run by
//
//     python port/android/test_disc_image.py
//
// with the disc tests.
package app.downhillrevived;

public class UpdatePageTest {
    private static int failures;

    private static void same(String page, String want) {
        String got = UpdatePage.safe(page);
        if (!got.equals(want)) {
            System.out.println("FAIL: " + page + " -> " + got + ", not " + want);
            failures++;
        }
    }

    public static void main(String[] args) {
        String repo = UpdatePage.REPOSITORY, releases = UpdatePage.RELEASES;
        same(repo + "releases/tag/v0.7.8", repo + "releases/tag/v0.7.8");
        same(releases, releases);
        same(null, releases);
        same("https://example.com/", releases);
        same("https://github.com/skuza-oss/downhill-revived-fork/", releases);
        // a browser resolves these outside the repository (https://github.com/attacker/...)
        same(repo + "../../attacker/fake/releases/latest", releases);
        same(repo + "%2e%2e/%2E%2E/attacker/fake", releases);
        same(repo + ".%2e/attacker", releases);
        same(repo + "releases\\..\\..\\attacker", releases);
        same(repo + "releases/tag/v1 x", releases);
        same(repo + "releases/\"x", releases);
        if (failures > 0) {
            System.out.println("UpdatePageTest: " + failures + " failed");
            System.exit(1);
        }
        System.out.println("UpdatePageTest: all passed");
    }
}
