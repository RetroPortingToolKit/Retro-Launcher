package org.retroportingtoolkit.launcher;

import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import org.libsdl.app.SDLActivity;

/** Android owns installation and upgrades; the native child lives in the APK. */
public final class LauncherActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] {"c++_shared", "SDL3", "main"};
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        try {
            // Assets are read with std::filesystem by the existing UI. Refresh
            // packaged resources on launch; user data is in separate directories.
            copyAssets("retcomm", new File(getFilesDir(), "assets"));
        } catch (IOException e) {
            throw new IllegalStateException("Cannot install launcher resources", e);
        }
        super.onCreate(savedInstanceState);
    }

    @Override protected String[] getArguments() {
        String root = getFilesDir().getAbsolutePath();
        String nativeDir = getApplicationInfo().nativeLibraryDir;
        nativeSetenv("HOME", root);
        nativeSetenv("XDG_CONFIG_HOME", root + "/config");
        nativeSetenv("XDG_DATA_HOME", root + "/data");
        nativeSetenv("XDG_CACHE_HOME", getCacheDir().getAbsolutePath());
        nativeSetenv("TMPDIR", getCacheDir().getAbsolutePath());
        nativeSetenv("RETCOMM_ASSET_DIR", root + "/assets");
        nativeSetenv("RETRO_CORE_RUNNER", nativeDir + "/libretro-core-runner.so");
        nativeSetenv("LD_LIBRARY_PATH", nativeDir);
        return new String[0];
    }

    private void copyAssets(String path, File target) throws IOException {
        String[] children = getAssets().list(path);
        if (children != null && children.length > 0) {
            if (!target.isDirectory() && !target.mkdirs()) {
                throw new IOException("Cannot create " + target);
            }
            for (String child : children) copyAssets(path + "/" + child, new File(target, child));
        } else {
            try (InputStream in = getAssets().open(path);
                 FileOutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[16384];
                int count;
                while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
            }
        }
    }
}
