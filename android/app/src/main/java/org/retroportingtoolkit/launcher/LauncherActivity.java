package org.retroportingtoolkit.launcher;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInstaller;
import android.database.Cursor;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** Android owns installation and upgrades; the native child lives in the APK. */
public final class LauncherActivity extends SDLActivity {
    /** In libmain (hub_touch.cpp): the on-screen keyboard's height in pixels. */
    private static native void nativeImeInset(int bottom);

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

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // SDL hides the bars with the legacy setSystemUiVisibility flags, which
        // Android 15+ ignores for edge-to-edge apps: the status bar stayed drawn
        // over the hub. Ask the insets controller instead; a swipe shows them
        // transiently.
        if (!hasFocus) return;
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars == null) return;
        bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        bars.hide(WindowInsets.Type.systemBars());
    }

    @Override protected SDLSurface createSDLSurface(Context context) {
        return new SDLSurface(context) {
            // SDL's safe area also counts the gesture-navigation edge zones,
            // which in landscape takes a strip off both sides. Taps there are
            // fine; only visible bars and the camera cutout hide content.
            @Override public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                Insets hidden = insets.getInsets(
                        WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                SDLActivity.onNativeInsetsChanged(hidden.left, hidden.right, hidden.top, hidden.bottom);
                // The keyboard is not part of the safe area: the hub slides the
                // frame up over it instead of laying out into what is left.
                nativeImeInset(insets.getInsets(WindowInsets.Type.ime()).bottom);
                return insets;
            }
        };
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
        // The .debug package: the release APK installs beside it, not over it, so
        // self-update only checks (self_update.cpp).
        boolean debuggable = (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        nativeSetenv("RETRO_ANDROID_DEBUGGABLE", debuggable ? "1" : "0");
        return new String[0];
    }

    /**
     * The hub's self-update (hub_android.cpp): hand a downloaded APK to the
     * package installer, which checks its signature against this app's, asks
     * the player, and replaces and restarts the app. Returns null once the
     * installer has it, else why not. Called on the update job's thread.
     */
    public String installApk(String path) {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(getPackageName());
        File apk = new File(path);
        int id;
        try {
            id = installer.createSession(params);
        } catch (IOException | RuntimeException e) {
            return e.toString();
        }
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buffer = new byte[1 << 16];
                int count;
                while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                session.fsync(out);
            }
            // Mutable: the installer fills in the status extras.
            PendingIntent status = PendingIntent.getBroadcast(this, id,
                    new Intent(this, InstallStatusReceiver.class),
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(status.getIntentSender());
            return null;
        } catch (IOException | RuntimeException e) {
            installer.abandonSession(id);
            return e.toString();
        }
    }

    /**
     * The name a file the picker returned goes by (hub_import.cpp): a content://
     * URI carries none of its own, and a ROM's extension is how the library
     * knows its platform. "" when the provider has no display name.
     */
    public String displayName(String uri) {
        try (Cursor c = getContentResolver().query(Uri.parse(uri),
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String name = c.getString(0);
                return name != null ? name : "";
            }
        } catch (RuntimeException e) {
            // An unreadable provider: no name, and the caller says so.
        }
        return "";
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
