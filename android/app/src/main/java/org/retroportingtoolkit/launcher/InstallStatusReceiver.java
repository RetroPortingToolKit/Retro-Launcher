package org.retroportingtoolkit.launcher;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.widget.Toast;

/**
 * The package installer's answer to LauncherActivity.installApk. Not exported:
 * only the PendingIntent that installApk created reaches it, so the intent it
 * starts is always the installer's own confirmation.
 */
public final class InstallStatusReceiver extends BroadcastReceiver {
    @SuppressWarnings("deprecation")  // getParcelableExtra(String, Class) is API 33; minSdk is 30
    @Override public void onReceive(Context context, Intent intent) {
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(confirm);
            }
            return;
        }
        // On success Android has already replaced and stopped this app.
        if (status != PackageInstaller.STATUS_SUCCESS) {
            String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            Toast.makeText(context, "Update not installed" + (message != null ? ": " + message : ""),
                    Toast.LENGTH_LONG).show();
        }
    }
}
