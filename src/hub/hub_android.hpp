#pragma once

namespace retcomm::hub {

// Android: route self-update's downloaded APK to LauncherActivity.installApk
// (set_apk_installer). Call once, before the first update check.
void android_register_apk_installer();

}  // namespace retcomm::hub
