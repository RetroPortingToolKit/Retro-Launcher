#pragma once

// The VRU Microphone's ears: an SDL3 recording stream (16 kHz mono S16, SDL
// converting from whatever the device gives), a level meter, and Vosk
// (libvosk, Apache-2.0; third_party/vosk/NOTICE.md) decoding it against the
// active phrase list on a worker thread. The worker reports SpeechEvents
// (hub_vru.hpp) that the SpeechMachine turns into messages for the core.
//
// libvosk is never linked: it is loaded at runtime (dlopen / LoadLibrary)
// from beside the hub, from RETCOMM_VOSK_DIR (a CMake option naming an
// unpacked vosk release), from <data dir>/vru, or from the system loader. A
// hub built or installed without it still runs; the panel says where it
// looked. The model (vosk-model-small-en-us-0.15, Apache-2.0, ~40 MB) is
// downloaded on first use into <data dir>/vru/models/, never bundled.

#include "hub/hub_vru.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace retcomm::hub::vru {

// ---- recording devices ----------------------------------------------------------

struct RecordingDevice {
    SDL_AudioDeviceID id = 0;
    std::string name;
};
// SDL_GetAudioRecordingDevices, with the audio subsystem brought up if it
// is not yet. Empty when the machine has none.
std::vector<RecordingDevice> recording_devices();
// The device named `name`, or the default recording device for "" or an
// unplugged name (*fell_back set).
SDL_AudioDeviceID recording_device_for(const std::string& name, bool* fell_back);

// ---- libvosk --------------------------------------------------------------------

// The file name on this OS: libvosk.so / libvosk.dylib / libvosk.dll.
const char* vosk_library_name();
// Where the hub looks, in order (for the panel's "expected at ...").
std::vector<fs::path> vosk_search_paths(const fs::path& exe_dir, const fs::path& data_dir);

// The loaded library, once per process. load() is idempotent; a failure is
// kept so the panel can say why.
class VoskLibrary {
public:
    static VoskLibrary& get();
    bool load(const fs::path& exe_dir, const fs::path& data_dir);
    bool loaded() const { return handle_ != nullptr; }
    const std::string& path() const { return path_; }   // where it was found
    const std::string& error() const { return error_; } // why not

    // The C API (vosk_api.h), the subset the hub uses.
    struct VoskModel;
    struct VoskRecognizer;
    VoskModel* (*model_new)(const char* path) = nullptr;
    void (*model_free)(VoskModel*) = nullptr;
    VoskRecognizer* (*recognizer_new)(VoskModel*, float sample_rate) = nullptr;
    VoskRecognizer* (*recognizer_new_grm)(VoskModel*, float sample_rate, const char* grammar) = nullptr;
    void (*recognizer_set_words)(VoskRecognizer*, int) = nullptr;
    int (*recognizer_accept_waveform_s)(VoskRecognizer*, const short* data, int samples) = nullptr;
    const char* (*recognizer_result)(VoskRecognizer*) = nullptr;
    const char* (*recognizer_partial_result)(VoskRecognizer*) = nullptr;
    const char* (*recognizer_final_result)(VoskRecognizer*) = nullptr;
    void (*recognizer_reset)(VoskRecognizer*) = nullptr;
    void (*recognizer_free)(VoskRecognizer*) = nullptr;
    void (*set_log_level)(int) = nullptr;

private:
    void* handle_ = nullptr;
    std::string path_, error_;
    bool tried_ = false;
};

// ---- the model ------------------------------------------------------------------

constexpr const char* kVoskModelName = "vosk-model-small-en-us-0.15";
constexpr const char* kVoskModelUrl =
    "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip";

fs::path vosk_models_dir(const fs::path& data_dir); // <data>/vru/models
fs::path vosk_model_dir(const fs::path& data_dir);  // <data>/vru/models/<kVoskModelName>
bool vosk_model_present(const fs::path& data_dir);

// Downloads and unpacks the model on a worker (the hub's HTTP and zip
// reader). One at a time; progress for the panel.
class ModelDownload {
public:
    ~ModelDownload();
    bool start(const fs::path& data_dir, std::string* error);
    bool running() const { return running_.load(); }
    // Bytes so far and the total (0 until known); the last error; done ok.
    std::uint64_t downloaded() const { return downloaded_.load(); }
    std::uint64_t total() const { return total_.load(); }
    bool unpacking() const { return unpacking_.load(); }
    std::string error() const;
    bool finished_ok() const { return finished_ok_.load(); }

private:
    std::thread worker_;
    std::atomic<bool> running_{false}, unpacking_{false}, finished_ok_{false};
    std::atomic<std::uint64_t> downloaded_{0}, total_{0};
    mutable std::mutex mu_;
    std::string error_;
};

// ---- the microphone -------------------------------------------------------------

// Recording and recognition on a worker thread. Without libvosk or a model
// the stream still runs, for the level meter, and `recognizer_status()` says
// what is missing.
class Microphone {
public:
    struct Config {
        std::string device;     // SDL name; "" = default
        fs::path model_dir;     // vosk_model_dir(); may be absent
        fs::path exe_dir, data_dir;
        // The phrases to decode against; empty = decode nothing (the meter
        // only) unless free_form, which decodes any English for the Test.
        std::vector<std::string> grammar;
        bool free_form = false;
    };
    Microphone() = default;
    ~Microphone();
    Microphone(const Microphone&) = delete;
    Microphone& operator=(const Microphone&) = delete;

    bool start(const Config& c, std::string* error);
    void stop();
    bool running() const { return running_.load(); }

    // The active phrases changed: the recognizer is rebuilt with them.
    void set_grammar(std::vector<std::string> phrases, bool free_form);

    int level() const { return level_.load(); }       // 0..100, the last chunk
    std::string device_name() const;                  // what was opened
    bool device_fell_back() const { return fell_back_; }
    // "ready" | "loading the model" | "libvosk not found (expected at ...)" |
    // "model not downloaded" | "no phrases to listen for" | an error.
    std::string recognizer_status() const;
    bool recognizer_ready() const { return recognizer_ready_.load(); }
    // The recognizer's running partial text (for the Test), latest.
    std::string partial() const;

    std::vector<SpeechEvent> take_events();

private:
    void run();
    void post(const SpeechEvent& e);
    void set_status(const std::string& s);

    Config cfg_;
    std::thread worker_;
    SDL_AudioStream* run_stream_ = nullptr; // the worker's, while it runs
    std::atomic<bool> running_{false}, stop_{false}, recognizer_ready_{false};
    std::atomic<int> level_{0};
    std::atomic<bool> grammar_changed_{false};
    bool fell_back_ = false;
    mutable std::mutex mu_;
    std::string device_name_, status_, partial_;
    std::vector<std::string> grammar_;
    bool free_form_ = false;
    std::vector<SpeechEvent> events_;
};

} // namespace retcomm::hub::vru
