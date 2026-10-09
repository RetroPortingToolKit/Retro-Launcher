#include "hub/hub_android.hpp"

#include "retcomm/self_update.hpp"

#include <SDL3/SDL_system.h>
#include <jni.h>

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

}  // namespace

void android_register_apk_installer() { set_apk_installer(install_apk); }

}  // namespace retcomm::hub
