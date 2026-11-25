package dev.nxnative.android;

import java.io.File;

/** OodleNeed on the JVM, driven by dm/tools/tests/test_android_oodle.py. */
public final class OodleNeedJvmTest {
    public static void main(String[] a) {
        System.out.println(OodleNeed.needed(new File(a[0]), new File(a[1])));
    }
}
