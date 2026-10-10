#include "hub/hub_android.hpp"

#include "retcomm/self_update.hpp"

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <mutex>
#include <string>

namespace retcomm::hub {
namespace {

// LauncherActivity.installApk, on the update job's thread (SDL attaches it to
// the VM). The method is looked up through the activity object: FindClass on a
// native thread searches the system class loader and would not find the app's
// classes.
std::string install_apk(const fs::path& apk) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) return "no JNI environment on this thread";
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) return "no activity";
    std::string why;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(cls, "installApk", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!method) {
        env->ExceptionClear();
        why = "LauncherActivity.installApk is missing";
    } else {
        jstring path = env->NewStringUTF(apk.string().c_str());
        auto result = static_cast<jstring>(env->CallObjectMethod(activity, method, path));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            why = "LauncherActivity.installApk threw";
        } else if (result) {
            const char* text = env->GetStringUTFChars(result, nullptr);
            why = text ? text : "LauncherActivity.installApk failed";
            if (text) env->ReleaseStringUTFChars(result, text);
            env->DeleteLocalRef(result);
        }
        env->DeleteLocalRef(path);
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return why;
}

// The folder pick in flight: one at a time (the Browse buttons disable while a
// pick is busy).
std::mutex g_folder_mu;
SDL_DialogFileCallback g_folder_cb = nullptr;
void* g_folder_user = nullptr;

}  // namespace

void android_show_folder_dialog(SDL_DialogFileCallback callback, void* userdata) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    jmethodID method = nullptr;
    jclass cls = nullptr;
    if (env && activity) {
        cls = env->GetObjectClass(activity);
        method = env->GetMethodID(cls, "pickFolder", "()V");
        if (!method) env->ExceptionClear();
    }
    if (!method) {
        if (cls) env->DeleteLocalRef(cls);
        if (activity) env->DeleteLocalRef(activity);
        callback(userdata, nullptr, -1);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_folder_mu);
        g_folder_cb = callback;
        g_folder_user = userdata;
    }
    env->CallVoidMethod(activity, method);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        std::lock_guard<std::mutex> lock(g_folder_mu);
        g_folder_cb = nullptr;
        callback(userdata, nullptr, -1);
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

void android_register_apk_installer() { set_apk_installer(install_apk); }

}  // namespace retcomm::hub

// LauncherActivity.nativeFolderPicked: the folder pick's answer. A path; "" when
// the player cancelled; null on failure.
extern "C" JNIEXPORT void JNICALL
Java_org_retroportingtoolkit_launcher_LauncherActivity_nativeFolderPicked(JNIEnv* env, jclass,
                                                                         jstring path) {
    SDL_DialogFileCallback cb;
    void* user;
    {
        std::lock_guard<std::mutex> lock(retcomm::hub::g_folder_mu);
        cb = retcomm::hub::g_folder_cb;
        user = retcomm::hub::g_folder_user;
        retcomm::hub::g_folder_cb = nullptr;
    }
    if (!cb) return;
    if (!path) {
        cb(user, nullptr, -1);
        return;
    }
    const char* text = env->GetStringUTFChars(path, nullptr);
    const std::string picked = text ? text : "";
    if (text) env->ReleaseStringUTFChars(path, text);
    const char* list[2] = {picked.empty() ? nullptr : picked.c_str(), nullptr};
    cb(user, list, 0);
}
