#include "hub/hub_import.hpp"

#include "retcomm/core_titles.hpp"
#include "retcomm/zip_extract.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace retcomm::hub {

namespace {

bool is_content_uri(const std::string& s) { return s.rfind("content://", 0) == 0; }

// A display name made safe to use as one path component.
std::string safe_component(std::string s) {
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    if (s == "." || s == "..") s.clear();
    return s;
}

#if defined(__ANDROID__)
// LauncherActivity.displayName(String): the provider's OpenableColumns
// DISPLAY_NAME for a content:// URI, "" when it has none.
std::string android_display_name(const std::string& uri) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return {};
    std::string out;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = cls ? env->GetMethodID(cls, "displayName", "(Ljava/lang/String;)Ljava/lang/String;")
                      : nullptr;
    if (m) {
        jstring juri = env->NewStringUTF(uri.c_str());
        auto jname = static_cast<jstring>(env->CallObjectMethod(activity, m, juri));
        if (!env->ExceptionCheck() && jname) {
            const char* utf = env->GetStringUTFChars(jname, nullptr);
            if (utf) out = utf;
            env->ReleaseStringUTFChars(jname, utf);
        }
        env->ExceptionClear();
        if (jname) env->DeleteLocalRef(jname);
        env->DeleteLocalRef(juri);
    }
    env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return out;
}
#endif

// title.json at the bundle's root, or in its one folder.
fs::path find_title_json(const fs::path& root) {
    std::error_code ec;
    if (fs::is_regular_file(root / "title.json", ec)) return root / "title.json";
    fs::path found;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        if (fs::is_regular_file(e.path() / "title.json", ec)) {
            if (!found.empty()) return {}; // two candidates: not one title
            found = e.path() / "title.json";
        }
    }
    return found;
}

bool valid_id(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace

std::string picked_name(const std::string& pick) {
    if (is_content_uri(pick)) {
#if defined(__ANDROID__)
        return safe_component(android_display_name(pick));
#else
        return {};
#endif
    }
    return safe_component(fs::path(pick).filename().string());
}

bool copy_picked(const std::string& pick, const fs::path& dest, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::error_code ec;
    if (!is_content_uri(pick)) {
        const fs::path src(pick);
        if (!fs::is_regular_file(src, ec)) return fail("not a file: " + pick);
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail(ec.message());
        return true;
    }
    // A content:// URI: only SDL's stream reaches it (Android's ContentResolver).
    SDL_IOStream* in = SDL_IOFromFile(pick.c_str(), "rb");
    if (!in) return fail(std::string("cannot open ") + pick + ": " + SDL_GetError());
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        SDL_CloseIO(in);
        return fail("cannot write " + dest.string());
    }
    // A read may come back short without being the end (Android's content
    // stream does): only 0 is an answer, and the status says which one.
    std::vector<char> buf(1 << 20);
    for (;;) {
        const size_t n = SDL_ReadIO(in, buf.data(), buf.size());
        if (n > 0) {
            out.write(buf.data(), static_cast<std::streamsize>(n));
            continue;
        }
        if (SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF) break;
        if (SDL_GetIOStatus(in) == SDL_IO_STATUS_NOT_READY) {
            SDL_Delay(1);
            continue;
        }
        const std::string err = SDL_GetError();
        SDL_CloseIO(in);
        out.close();
        fs::remove(dest, ec);
        return fail("read failed: " + (err.empty() ? std::string("unknown error") : err));
    }
    SDL_CloseIO(in);
    out.close();
    if (!out) {
        fs::remove(dest, ec);
        return fail("write failed: " + dest.string());
    }
    return true;
}

bool install_title_bundle(const std::string& pick, const fs::path& titles_root,
                          const fs::path& scratch, InstalledTitle* out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::error_code ec;
    fs::create_directories(titles_root, ec);
    if (ec) return fail("cannot create " + titles_root.string() + ": " + ec.message());

    // The zip itself: read in place when it is a path, copied out of a
    // content:// URI first (miniz reads a file).
    fs::path zip = pick;
    fs::path zip_copy;
    if (is_content_uri(pick)) {
        fs::create_directories(scratch, ec);
        zip_copy = scratch / "title-bundle.zip";
        std::string err;
        if (!copy_picked(pick, zip_copy, &err)) return fail(err);
        zip = zip_copy;
    }
    // Unpacked beside its final place, so the move into it is a rename.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path staging = titles_root / (".staging-" + std::to_string(stamp));
    auto cleanup = [&] {
        std::error_code e;
        fs::remove_all(staging, e);
        if (!zip_copy.empty()) fs::remove(zip_copy, e);
    };
    std::string err;
    if (!retcomm::zip::extract_file(zip, staging, &err)) {
        cleanup();
        return fail("not a readable zip: " + err);
    }
    const fs::path json = find_title_json(staging);
    if (json.empty()) {
        cleanup();
        return fail("the bundle has no title.json (at its root, or in its one folder)");
    }
    CoreTitle ct;
    if (!read_core_title(json, ct, &err)) {
        cleanup();
        return fail(err);
    }
    if (!valid_id(ct.id)) {
        cleanup();
        return fail("its title.json id '" + ct.id + "' is not [a-z0-9_-]+");
    }
    // Everything title.json names must be inside the bundle (read_core_title
    // checked the core exists; it must also be under the bundle's folder).
    const fs::path bundle_root = json.parent_path();
    auto inside = [&](const fs::path& p) {
        const auto rel = p.lexically_normal().lexically_relative(bundle_root.lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    };
    if (!inside(ct.library) || (!ct.package.empty() && !inside(ct.package))) {
        cleanup();
        return fail("its title.json names files outside the bundle");
    }
    if (!ct.package.empty() && !fs::is_regular_file(ct.package, ec)) {
        cleanup();
        return fail("its game package " + ct.package.filename().string() + " is missing");
    }
    const fs::path final_dir = titles_root / ct.id;
    fs::remove_all(final_dir, ec);
    fs::rename(bundle_root, final_dir, ec);
    if (ec) {
        cleanup();
        return fail("cannot install into " + final_dir.string() + ": " + ec.message());
    }
    cleanup();
    if (out) {
        out->id = ct.id;
        out->name = ct.name;
        out->manifest = final_dir / "title.json";
    }
    return true;
}

} // namespace retcomm::hub
