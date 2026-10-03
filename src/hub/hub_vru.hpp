#pragma once

// The VRU Microphone, the part that needs no microphone: what n64lle's
// companion (tools/vru_client.py, docs/VRU.md) did between the recognizer and
// the guest, run natively by the hub.
//
//   vocabulary   the title's locally generated phoneme -> text table
//                (<title dir>/vru_vocabulary.json, or game.toml's [vru]
//                vocabulary = <path>). ROM-derived, never in a repository.
//   dictionary   what the guest uploaded, as the core reports it over the
//                accessory data link (one JSON line per change): epoch,
//                listening, capture_id, and each word's slot, enabled flag
//                and phoneme string.
//   machine      the companion's LiveSpeech: a recognizer onset while the
//                guest listens binds to its capture_id and sends `start`;
//                progress carries the level; a final text that is an ENABLED
//                active phrase sends `result` with its slot; anything else
//                `cancel`. The core commits a result only after Z release.
//
// Pure: no SDL, no recognizer, no link -- tests/hub_vru_test.cpp drives it
// with scripted dictionaries and events. The microphone side is
// hub_vru_mic.hpp.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace retcomm::hub::vru {

namespace fs = std::filesystem;

// The companion's normalize(): lower-case alphanumeric words, one space
// between ("Good  Morning!" -> "good morning").
std::string normalize_phrase(const std::string& text);

// ---- vocabulary -----------------------------------------------------------------

struct VocabularyEntry {
    std::string text;   // normalized
    std::string sounds; // the phoneme string, lower-case hex, as the guest uploads it
};

struct Vocabulary {
    std::vector<VocabularyEntry> entries;
    bool loaded() const { return !entries.empty(); }
    // Every phrase this phoneme string spells, sorted; empty for one the
    // title's table does not hold.
    std::vector<std::string> names(const std::string& sounds) const;
    // Every phrase, sorted, once.
    std::vector<std::string> all_names() const;
};

// Schema 1, en-US: {"schema":1,"language":"en-US","entries":[{"text","sounds"}]}.
bool parse_vocabulary(const std::string& json_text, Vocabulary& out, std::string* error);
bool load_vocabulary(const fs::path& file, Vocabulary& out, std::string* error);

// Where a title keeps its vocabulary: game.toml `[vru] vocabulary = <path>`
// (relative to the title dir), else <title dir>/vru_vocabulary.json. The
// file need not exist; the caller says so, naming this path.
fs::path vocabulary_path(const fs::path& title_dir);

// ---- the guest's dictionary ------------------------------------------------------

struct DictionaryWord {
    std::uint32_t slot = 0;
    bool enabled = false;
    std::string sounds; // lower-case hex
};

// The core's `vru` object, as accessory_notify carries it.
struct Dictionary {
    bool attached = false;
    std::string language;
    std::uint64_t epoch = 0;
    bool listening = false;
    int mode = 0;
    bool pending = false;
    int expected = 0;
    std::uint64_t capture_id = 0;
    bool speech = false;
    std::vector<DictionaryWord> words;
};

// One JSON line (a trailing newline is fine). A `vru` wrapper object, as the
// debug endpoint replies, is unwrapped.
bool parse_dictionary(const std::string& json_line, Dictionary& out, std::string* error);

// The phrases the guest will take right now: normalized text -> slot, from
// the ENABLED words the vocabulary knows. A phrase two words spell keeps the
// first slot (the companion's rule).
std::map<std::string, std::uint32_t> active_phrases(const Dictionary& d, const Vocabulary& v);

// ---- the speech state machine --------------------------------------------------

// What the recognizer reports. `utterance` ties the events of one stretch
// of speech together (the mic's own counter).
struct SpeechEvent {
    enum Kind { Start, Progress, Final, Cancel };
    Kind kind = Start;
    std::uint64_t utterance = 0;
    std::uint32_t duration_ms = 0; // Progress / Final
    int level = 0;                 // Progress, 0..100
    std::string text;              // Final; empty = nothing recognized
    float confidence = 1.f;        // Final
};

constexpr float kMinConfidence = 0.45f; // below it a final is unclear: cancel

class SpeechMachine {
public:
    // The vocabulary must outlive the machine.
    void set_vocabulary(const Vocabulary* v) { vocabulary_ = v; }

    // A dictionary arrived from the core. A new epoch drops any capture in
    // progress: its slots and capture_id are stale.
    void dictionary(const Dictionary& d);
    const Dictionary& current() const { return dict_; }
    bool have_dictionary() const { return have_dict_; }

    // The grammar the recognizer should run: the active phrases, sorted.
    std::vector<std::string> active_names() const;

    struct Outcome {
        std::vector<std::string> messages; // JSON lines for the core, in order
        std::string note;                  // one line for the player, or empty
    };
    // One recognizer event. Ambient speech (the guest not listening, or an
    // utterance this machine never bound) produces nothing.
    Outcome event(const SpeechEvent& e);

    // A capture is bound: `start` was sent and no final/cancel yet.
    bool bound() const { return bound_; }
    // Drop the bound capture, telling the core (the mic stopped).
    Outcome abandon();

    // The messages themselves, for the hub's own sends.
    static std::string speech_message(std::uint64_t epoch, std::uint64_t capture_id,
                                      const char* event, std::uint32_t duration_ms = 0,
                                      int level = -1, std::int64_t slot = -1);
    static std::string submit_message(std::uint64_t epoch, std::uint32_t slot,
                                      std::uint32_t duration_ms);

private:
    const Vocabulary* vocabulary_ = nullptr;
    Dictionary dict_;
    bool have_dict_ = false;
    bool bound_ = false;
    std::uint64_t utterance_ = 0;
    std::uint64_t epoch_ = 0, capture_id_ = 0;
    std::map<std::string, std::uint32_t> phrases_; // as active when bound
};

} // namespace retcomm::hub::vru
