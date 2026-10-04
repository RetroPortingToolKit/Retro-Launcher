#include "hub/hub_vru_mic.hpp"

#include "retcomm/http.hpp"
#include "retcomm/zip_extract.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace retcomm::hub::vru {

using json = nlohmann::json;

namespace {

constexpr int kSampleRate = 16000;
// 50 ms of audio per chunk: the meter at 20 Hz, Vosk fed in pieces it likes.
constexpr int kChunkSamples = kSampleRate / 20;
// An onset: this level for two chunks running (RMS / 40, so ~500 RMS).
constexpr int kOnsetLevel = 12;
// A stretch of speech the recognizer has not closed by itself is forced
// closed at this length: a held key with a fan behind it must not listen
// forever. Hey You Pikachu's phrases are a second or two.
constexpr std::uint32_t kMaxUtteranceMs = 8000;
constexpr std::uint32_t kProgressEveryMs = 100;

bool ensure_audio() {
    return SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO);
}

} // namespace

// ---- recording devices ----------------------------------------------------------

std::vector<RecordingDevice> recording_devices() {
    std::vector<RecordingDevice> out;
    if (!ensure_audio()) return out;
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&n);
    for (int i = 0; ids && i < n; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        out.push_back({ids[i], name ? name : ("device " + std::to_string(ids[i]))});
    }
    SDL_free(ids);
    return out;
}

SDL_AudioDeviceID recording_device_for(const std::string& name, bool* fell_back) {
    if (fell_back) *fell_back = false;
    if (name.empty()) return SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    for (const RecordingDevice& d : recording_devices())
        if (d.name == name) return d.id;
    if (fell_back) *fell_back = true;
    return SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
}

// ---- libvosk --------------------------------------------------------------------

const char* vosk_library_name() {
#if defined(_WIN32)
    return "libvosk.dll";
#elif defined(__APPLE__)
    return "libvosk.dylib";
#else
    return "libvosk.so";
#endif
}

std::vector<fs::path> vosk_search_paths(const fs::path& exe_dir, const fs::path& data_dir) {
    std::vector<fs::path> out;
    const fs::path name = vosk_library_name();
    if (!exe_dir.empty()) {
        out.push_back(exe_dir / name);
        out.push_back(exe_dir / ".." / "lib" / name);
    }
#if defined(RETCOMM_VOSK_DIR)
    {
        // The unpacked release: libvosk at its root, or in the one directory
        // the zip holds (vosk-linux-x86_64-0.3.45/libvosk.so).
        const fs::path dir = fs::u8path(RETCOMM_VOSK_DIR);
        out.push_back(dir / name);
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec)) out.push_back(e.path() / name);
    }
#endif
    if (!data_dir.empty()) out.push_back(data_dir / "vru" / name);
    out.push_back(name); // the system loader's own search
    return out;
}

VoskLibrary& VoskLibrary::get() {
    static VoskLibrary lib;
    return lib;
}

bool VoskLibrary::load(const fs::path& exe_dir, const fs::path& data_dir) {
    if (tried_) return handle_ != nullptr;
    tried_ = true;
    const std::vector<fs::path> paths = vosk_search_paths(exe_dir, data_dir);
    // A file that was there but would not load (a libvosk built against a
    // BLAS this machine lacks, say) is the more useful thing to report.
    std::string refused;
    for (const fs::path& p : paths) {
        std::error_code ec;
        const bool bare = !p.has_parent_path();
        if (!bare && !fs::is_regular_file(p, ec)) continue;
#if defined(_WIN32)
        HMODULE h = bare ? LoadLibraryW(p.wstring().c_str())
                         : LoadLibraryExW(p.wstring().c_str(), nullptr,
                                          LOAD_WITH_ALTERED_SEARCH_PATH);
        handle_ = reinterpret_cast<void*>(h);
        if (!handle_ && !bare) refused = p.string() + " (error " + std::to_string(GetLastError()) + ")";
#else
        handle_ = dlopen(p.string().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_ && !bare) {
            const char* why = dlerror();
            refused = p.string() + (why ? std::string(" (") + why + ")" : std::string());
        }
#endif
        if (handle_) {
            path_ = p.string();
            break;
        }
    }
    if (!handle_) {
        std::string where;
        for (const fs::path& p : paths) where += (where.empty() ? "" : ", ") + p.string();
        error_ = refused.empty()
                     ? "libvosk not found (expected at " + where + ")"
                     : "libvosk would not load: " + refused;
        return false;
    }
    auto sym = [&](const char* n) -> void* {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), n));
#else
        return dlsym(handle_, n);
#endif
    };
    bool ok = true;
    auto bind = [&](auto& fn, const char* n) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(sym(n));
        if (!fn) {
            ok = false;
            error_ = path_ + " lacks " + n;
        }
    };
    bind(model_new, "vosk_model_new");
    bind(model_free, "vosk_model_free");
    bind(recognizer_new, "vosk_recognizer_new");
    bind(recognizer_new_grm, "vosk_recognizer_new_grm");
    bind(recognizer_set_words, "vosk_recognizer_set_words");
    bind(recognizer_accept_waveform_s, "vosk_recognizer_accept_waveform_s");
    bind(recognizer_result, "vosk_recognizer_result");
    bind(recognizer_partial_result, "vosk_recognizer_partial_result");
    bind(recognizer_final_result, "vosk_recognizer_final_result");
    bind(recognizer_reset, "vosk_recognizer_reset");
    bind(recognizer_free, "vosk_recognizer_free");
    bind(set_log_level, "vosk_set_log_level");
    if (!ok) {
#if defined(_WIN32)
        FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
        return false;
    }
    set_log_level(-1); // Kaldi's chatter stays off the hub's stderr
    return true;
}

// ---- the model ------------------------------------------------------------------

fs::path vosk_models_dir(const fs::path& data_dir) { return data_dir / "vru" / "models"; }
fs::path vosk_model_dir(const fs::path& data_dir) { return vosk_models_dir(data_dir) / kVoskModelName; }

bool vosk_model_present(const fs::path& data_dir) {
    std::error_code ec;
    const fs::path dir = vosk_model_dir(data_dir);
    // The acoustic model and the graph: what every Vosk model directory holds.
    return fs::is_regular_file(dir / "am" / "final.mdl", ec) &&
           fs::is_directory(dir / "graph", ec);
}

ModelDownload::~ModelDownload() {
    if (worker_.joinable()) worker_.join();
}

std::string ModelDownload::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

bool ModelDownload::start(const fs::path& data_dir, std::string* error) {
    if (running_.load()) {
        if (error) *error = "a download is already running";
        return false;
    }
    if (worker_.joinable()) worker_.join();
    running_ = true;
    finished_ok_ = false;
    unpacking_ = false;
    downloaded_ = 0;
    total_ = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_.clear();
    }
    worker_ = std::thread([this, data_dir] {
        const fs::path models = vosk_models_dir(data_dir);
        const fs::path zip = models / (std::string(kVoskModelName) + ".zip");
        std::error_code ec;
        fs::create_directories(models, ec);
        std::string err;
        const bool got = retcomm::http_download(
            kVoskModelUrl, zip, &err, {},
            [this](std::uint64_t done, std::uint64_t total) {
                downloaded_ = done;
                total_ = total;
            });
        if (got) {
            unpacking_ = true;
            // A half-unpacked model from an earlier failure must not be taken
            // for a whole one.
            fs::remove_all(vosk_model_dir(data_dir), ec);
            if (!retcomm::zip::extract_file(zip, models, &err)) {
                fs::remove_all(vosk_model_dir(data_dir), ec);
            } else if (!vosk_model_present(data_dir)) {
                err = "the archive did not hold " + std::string(kVoskModelName) + "/am/final.mdl";
                fs::remove_all(vosk_model_dir(data_dir), ec);
            }
            fs::remove(zip, ec);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = got && vosk_model_present(data_dir) ? std::string() : err;
        }
        finished_ok_ = got && vosk_model_present(data_dir);
        unpacking_ = false;
        running_ = false;
    });
    return true;
}

// ---- the microphone -------------------------------------------------------------

Microphone::~Microphone() { stop(); }

std::string Microphone::device_name() const {
    std::lock_guard<std::mutex> lock(mu_);
    return device_name_;
}

std::string Microphone::recognizer_status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

std::string Microphone::partial() const {
    std::lock_guard<std::mutex> lock(mu_);
    return partial_;
}

void Microphone::set_status(const std::string& s) {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = s;
}

void Microphone::post(const SpeechEvent& e) {
    std::lock_guard<std::mutex> lock(mu_);
    events_.push_back(e);
}

std::vector<SpeechEvent> Microphone::take_events() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SpeechEvent> out;
    out.swap(events_);
    return out;
}

void Microphone::set_grammar(std::vector<std::string> phrases, bool free_form) {
    std::lock_guard<std::mutex> lock(mu_);
    if (grammar_ == phrases && free_form_ == free_form) return;
    grammar_ = std::move(phrases);
    free_form_ = free_form;
    grammar_changed_ = true;
}

bool Microphone::start(const Config& c, std::string* error) {
    stop();
    if (!ensure_audio()) {
        if (error) *error = std::string("SDL audio: ") + SDL_GetError();
        return false;
    }
    cfg_ = c;
    {
        std::lock_guard<std::mutex> lock(mu_);
        grammar_ = c.grammar;
        free_form_ = c.free_form;
        events_.clear();
        partial_.clear();
        status_ = "starting";
    }
    grammar_changed_ = true;
    stop_ = false;
    recognizer_ready_ = false;
    level_ = 0;
    const SDL_AudioDeviceID dev = recording_device_for(c.device, &fell_back_);
    const char* name = dev == SDL_AUDIO_DEVICE_DEFAULT_RECORDING ? "default recording device"
                                                                 : SDL_GetAudioDeviceName(dev);
    {
        std::lock_guard<std::mutex> lock(mu_);
        device_name_ = name ? name : "recording device";
    }
    // The stream is opened on the worker: a device that takes a while to
    // come up must not stall the hub's frame.
    running_ = true;
    worker_ = std::thread([this, dev] {
        SDL_AudioSpec want{};
        want.format = SDL_AUDIO_S16;
        want.channels = 1;
        want.freq = kSampleRate;
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(dev, &want, nullptr, nullptr);
        if (!stream) {
            set_status(std::string("cannot open the recording device: ") + SDL_GetError());
            running_ = false;
            return;
        }
        SDL_ResumeAudioStreamDevice(stream);
        run_stream_ = stream;
        run();
        SDL_DestroyAudioStream(stream);
        run_stream_ = nullptr;
        running_ = false;
    });
    return true;
}

void Microphone::stop() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    running_ = false;
    recognizer_ready_ = false;
    level_ = 0;
}

void Microphone::run() {
    VoskLibrary& vosk = VoskLibrary::get();
    const bool have_lib = vosk.load(cfg_.exe_dir, cfg_.data_dir);
    VoskLibrary::VoskModel* model = nullptr;
    VoskLibrary::VoskRecognizer* rec = nullptr;
    std::error_code ec;
    if (!have_lib) {
        set_status(vosk.error());
    } else if (!fs::is_directory(cfg_.model_dir, ec)) {
        set_status("model not downloaded");
    } else {
        set_status("loading the model");
        model = vosk.model_new(cfg_.model_dir.string().c_str());
        if (!model) set_status("the model at " + cfg_.model_dir.string() + " did not load");
    }

    using clock = std::chrono::steady_clock;
    std::vector<short> chunk(kChunkSamples);
    std::uint64_t utterance = 0;
    bool speaking = false;
    clock::time_point onset, last_progress;
    int loud_chunks = 0;
    std::vector<std::string> grammar;
    bool free_form = false;

    auto rebuild = [&] {
        if (rec) {
            vosk.recognizer_free(rec);
            rec = nullptr;
        }
        if (!model) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            grammar = grammar_;
            free_form = free_form_;
        }
        if (free_form) {
            rec = vosk.recognizer_new(model, static_cast<float>(kSampleRate));
        } else if (!grammar.empty()) {
            // The active phrases, plus [unk] so anything else decodes as
            // unknown rather than as the nearest phrase.
            json g = json::array();
            for (const std::string& p : grammar) g.push_back(p);
            g.push_back("[unk]");
            rec = vosk.recognizer_new_grm(model, static_cast<float>(kSampleRate), g.dump().c_str());
        }
        if (rec) vosk.recognizer_set_words(rec, 1);
        recognizer_ready_ = rec != nullptr;
        set_status(rec ? "ready" : "no phrases to listen for");
    };

    auto finish = [&](const char* result_json, bool forced) {
        // {"text": "...", "result": [{"conf": .., "word": ..}, ...]}
        SpeechEvent e;
        e.kind = SpeechEvent::Final;
        e.utterance = utterance;
        e.duration_ms = speaking ? static_cast<std::uint32_t>(
                                       std::chrono::duration_cast<std::chrono::milliseconds>(
                                           clock::now() - onset)
                                           .count())
                                 : 0;
        json r = result_json ? json::parse(result_json, nullptr, false) : json();
        if (r.is_object()) {
            e.text = r.value("text", std::string());
            const auto words = r.find("result");
            if (words != r.end() && words->is_array() && !words->empty()) {
                double sum = 0;
                for (const json& w : *words) sum += w.value("conf", 1.0);
                e.confidence = static_cast<float>(sum / static_cast<double>(words->size()));
            }
        }
        if (e.text.find("[unk]") != std::string::npos) e.text.clear();
        {
            std::lock_guard<std::mutex> lock(mu_);
            partial_ = e.text.empty() ? (forced ? "(nothing recognized)" : "") : e.text;
        }
        if (speaking) {
            post(e);
        } else if (!e.text.empty()) {
            // The recognizer heard words the meter never saw rise: report
            // the onset now, then the result, so the machine can bind.
            ++utterance;
            e.utterance = utterance;
            SpeechEvent s;
            s.kind = SpeechEvent::Start;
            s.utterance = utterance;
            post(s);
            post(e);
        }
        speaking = false;
        loud_chunks = 0;
    };

    while (!stop_.load()) {
        if (grammar_changed_.exchange(false)) {
            if (speaking) {
                SpeechEvent c;
                c.kind = SpeechEvent::Cancel;
                c.utterance = utterance;
                post(c);
                speaking = false;
            }
            rebuild();
        }
        const int want = static_cast<int>(chunk.size() * sizeof(short));
        if (SDL_GetAudioStreamAvailable(run_stream_) < want) {
            SDL_Delay(5);
            continue;
        }
        const int got = SDL_GetAudioStreamData(run_stream_, chunk.data(), want);
        if (got <= 0) {
            SDL_Delay(5);
            continue;
        }
        const int samples = got / static_cast<int>(sizeof(short));
        double sq = 0;
        for (int i = 0; i < samples; ++i) sq += double(chunk[i]) * double(chunk[i]);
        const double rms = std::sqrt(sq / std::max(1, samples));
        const int level = static_cast<int>(std::clamp(rms / 40.0, 0.0, 100.0));
        level_ = level;
        if (!rec) continue;

        const clock::time_point now = clock::now();
        loud_chunks = level >= kOnsetLevel ? loud_chunks + 1 : 0;
        const int done = vosk.recognizer_accept_waveform_s(rec, chunk.data(), samples);
        if (done < 0) {
            set_status("the recognizer failed on a chunk");
            continue;
        }
        std::string partial;
        if (done == 0) {
            json p = json::parse(vosk.recognizer_partial_result(rec), nullptr, false);
            if (p.is_object()) partial = p.value("partial", std::string());
            std::lock_guard<std::mutex> lock(mu_);
            partial_ = partial;
        }
        if (!speaking && (loud_chunks >= 2 || !partial.empty())) {
            speaking = true;
            onset = now;
            last_progress = now;
            ++utterance;
            SpeechEvent s;
            s.kind = SpeechEvent::Start;
            s.utterance = utterance;
            post(s);
        }
        if (done == 1) {
            finish(vosk.recognizer_result(rec), false);
            continue;
        }
        if (!speaking) continue;
        const auto ms = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - onset).count());
        if (ms >= kMaxUtteranceMs) {
            finish(vosk.recognizer_final_result(rec), true);
            continue;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress).count() >=
            kProgressEveryMs) {
            last_progress = now;
            SpeechEvent p;
            p.kind = SpeechEvent::Progress;
            p.utterance = utterance;
            p.duration_ms = ms;
            p.level = level;
            post(p);
        }
    }
    if (speaking) {
        SpeechEvent c;
        c.kind = SpeechEvent::Cancel;
        c.utterance = utterance;
        post(c);
    }
    if (rec) vosk.recognizer_free(rec);
    if (model) vosk.model_free(model);
}

} // namespace retcomm::hub::vru
