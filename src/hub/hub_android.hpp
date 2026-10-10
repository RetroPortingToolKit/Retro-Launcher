#pragma once

#include <SDL3/SDL_dialog.h>

namespace retcomm::hub {

// Android: route self-update's downloaded APK to LauncherActivity.installApk
// (set_apk_installer). Call once, before the first update check.
void android_register_apk_installer();

// Android's folder picker (SDL has none there): the same callback contract as
// SDL_ShowOpenFolderDialog. The callback gets the picked folder's path, a
// list holding only the terminator when the player cancelled, or a null list
// on failure, and is always called exactly once, from the UI thread.
void android_show_folder_dialog(SDL_DialogFileCallback callback, void* userdata);

}  // namespace retcomm::hub
