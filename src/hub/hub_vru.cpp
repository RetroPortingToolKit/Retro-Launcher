#include "hub/hub_vru.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace retcomm::hub::vru {

using json = nlohmann::json;

std::string normalize_phrase(const std::string& text) {
    std::string out, word;
    auto flush = [&] {
        if (word.empty()) return;
        if (!out.empty()) out += ' ';
        out += word;
        word.clear();
    };
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 0x80) word += static_cast<char>(std::tolower(u));
        else flush();
    }
    flush();
    return out;
}

namespace {

std::string lower_hex(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// `key = value` of one [section] of a small TOML file (the [vru] section of
// game.toml): a bare or a double-quoted string, nothing cleverer.
std::string toml_value(const fs::path& file, const std::string& want_section,
                       const std::string& want_key) {
    std::ifstream in(file);
    std::string line, section;
    while (std::getline(in, line)) {
        const auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        if (line[b] == '[') {
            const auto e = line.find(']', b);
            section = e == std::string::npos ? "" : line.substr(b + 1, e - b - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (section != want_section || eq == std::string::npos) continue;
        std::string k = line.substr(b, eq - b);
        while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
        if (k != want_key) continue;
        std::string v = line.substr(eq + 1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
        if (!v.empty() && v.front() == '"') {
            const auto close = v.find('"', 1);
            return close == std::string::npos ? std::string() : v.substr(1, close - 1);
        }
        v = v.substr(0, v.find(" #"));
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        return v;
    }
    return {};
}

} // namespace

// ---- vocabulary -----------------------------------------------------------------

std::vector<std::string> Vocabulary::names(const std::string& sounds) const {
    const std::string key = lower_hex(sounds);
    std::set<std::string> out;
    for (const VocabularyEntry& e : entries)
        if (e.sounds == key) out.insert(e.text);
    return {out.begin(), out.end()};
}

std::vector<std::string> Vocabulary::all_names() const {
    std::set<std::string> out;
    for (const VocabularyEntry& e : entries) out.insert(e.text);
    return {out.begin(), out.end()};
}

bool parse_vocabulary(const std::string& json_text, Vocabulary& out, std::string* error) {
    out = Vocabulary{};
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    json data = json::parse(json_text, nullptr, false);
    if (!data.is_object()) return fail("not a JSON object");
    if (data.value("schema", 0) != 1 || data.value("language", std::string()) != "en-US")
        return fail("expected an English schema-1 vocabulary");
    const auto entries = data.find("entries");
    if (entries == data.end() || !entries->is_array()) return fail("no entries array");
    for (const json& e : *entries) {
        if (!e.is_object() || !e.contains("text") || !e.contains("sounds") ||
            !e["text"].is_string() || !e["sounds"].is_string())
            return fail("an entry without text and sounds strings");
        VocabularyEntry v;
        v.text = normalize_phrase(e["text"].get<std::string>());
        v.sounds = lower_hex(e["sounds"].get<std::string>());
        if (v.text.empty() || v.sounds.empty()) continue;
        out.entries.push_back(std::move(v));
    }
    if (out.entries.empty()) return fail("no usable entries");
    return true;
}

bool load_vocabulary(const fs::path& file, Vocabulary& out, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        if (error) *error = "not found";
        return false;
    }
    return parse_vocabulary(read_text(file), out, error);
}

fs::path vocabulary_path(const fs::path& title_dir) {
    const std::string set = toml_value(title_dir / "game.toml", "vru", "vocabulary");
    if (!set.empty()) {
        const fs::path p = fs::u8path(set);
        return p.is_absolute() ? p : title_dir / p;
    }
    return title_dir / "vru_vocabulary.json";
}

// ---- the guest's dictionary ------------------------------------------------------

bool parse_dictionary(const std::string& json_line, Dictionary& out, std::string* error) {
    out = Dictionary{};
    json data = json::parse(json_line, nullptr, false);
    if (data.is_object() && data.contains("vru") && data["vru"].is_object()) data = data["vru"];
    if (!data.is_object()) {
        if (error) *error = "not a JSON object";
        return false;
    }
    auto flag = [&](const char* k) {
        const auto it = data.find(k);
        if (it == data.end()) return false;
        if (it->is_boolean()) return it->get<bool>();
        if (it->is_number()) return it->get<double>() != 0;
        return false;
    };
    auto num = [&](const char* k, std::uint64_t def) -> std::uint64_t {
        const auto it = data.find(k);
        if (it == data.end() || !it->is_number()) return def;
        return it->get<std::uint64_t>();
    };
    out.attached = flag("attached");
    out.language = data.value("language", std::string());
    out.epoch = num("epoch", 0);
    out.listening = flag("listening");
    out.mode = static_cast<int>(num("mode", 0));
    out.pending = flag("pending");
    out.expected = static_cast<int>(num("expected", 0));
    out.capture_id = num("capture_id", 0);
    out.speech = flag("speech");
    const auto words = data.find("words");
    if (words != data.end() && words->is_array()) {
        for (const json& w : *words) {
            if (!w.is_object()) continue;
            DictionaryWord d;
            d.slot = w.value("slot", 0u);
            const auto en = w.find("enabled");
            d.enabled = en != w.end() && (en->is_boolean() ? en->get<bool>()
                                                             : en->is_number() && en->get<double>() != 0);
            d.sounds = lower_hex(w.value("sounds", std::string()));
            out.words.push_back(std::move(d));
        }
    }
    return true;
}

std::map<std::string, std::uint32_t> active_phrases(const Dictionary& d, const Vocabulary& v) {
    std::map<std::string, std::uint32_t> out;
    for (const DictionaryWord& w : d.words) {
        if (!w.enabled) continue;
        for (const std::string& name : v.names(w.sounds)) out.emplace(name, w.slot);
    }
    return out;
}

// ---- the speech state machine --------------------------------------------------

std::string SpeechMachine::speech_message(std::uint64_t epoch, std::uint64_t capture_id,
                                          const char* event, std::uint32_t duration_ms,
                                          int level, std::int64_t slot) {
    json m = {{"cmd", "vru_speech"}, {"epoch", epoch}, {"capture_id", capture_id}, {"event", event}};
    if (duration_ms) m["duration_ms"] = duration_ms;
    if (level >= 0) m["level"] = std::clamp(level, 0, 100);
    if (slot >= 0) m["slot"] = slot;
    return m.dump() + "\n";
}

std::string SpeechMachine::submit_message(std::uint64_t epoch, std::uint32_t slot,
                                          std::uint32_t duration_ms) {
    json m = {{"cmd", "vru_submit"}, {"epoch", epoch}, {"slot", slot}, {"duration_ms", duration_ms}};
    return m.dump() + "\n";
}

void SpeechMachine::dictionary(const Dictionary& d) {
    const bool new_epoch = have_dict_ && d.epoch != dict_.epoch;
    dict_ = d;
    have_dict_ = true;
    // The capture bound under the old epoch is stale: the core has already
    // expired it (docs/VRU.md: clear, upload, mask changes and state loads
    // expire old epochs), so nothing is sent for it.
    if (new_epoch && bound_) bound_ = false;
}

std::vector<std::string> SpeechMachine::active_names() const {
    if (!have_dict_ || !vocabulary_) return {};
    std::vector<std::string> out;
    for (const auto& [name, slot] : active_phrases(dict_, *vocabulary_)) out.push_back(name);
    return out;
}

SpeechMachine::Outcome SpeechMachine::event(const SpeechEvent& e) {
    Outcome o;
    if (e.kind == SpeechEvent::Start) {
        bound_ = false;
        // Ambient speech cannot become a later command: only an onset while
        // the guest listens binds to its capture.
        if (!have_dict_ || !dict_.listening || !vocabulary_) return o;
        bound_ = true;
        utterance_ = e.utterance;
        epoch_ = dict_.epoch;
        capture_id_ = dict_.capture_id;
        phrases_ = active_phrases(dict_, *vocabulary_);
        o.messages.push_back(speech_message(epoch_, capture_id_, "start"));
        o.note = "Listening. Keep Z held while speaking.";
        return o;
    }
    if (!bound_ || e.utterance != utterance_) return o;
    switch (e.kind) {
        case SpeechEvent::Progress:
            o.messages.push_back(
                speech_message(epoch_, capture_id_, "progress", e.duration_ms, e.level));
            break;
        case SpeechEvent::Final: {
            bound_ = false;
            const std::string text = normalize_phrase(e.text);
            const auto it = text.empty() ? phrases_.end() : phrases_.find(text);
            if (e.confidence < kMinConfidence) {
                o.messages.push_back(speech_message(epoch_, capture_id_, "cancel"));
                o.note = "Speech was unclear. Hold Z and try again.";
            } else if (it == phrases_.end()) {
                o.messages.push_back(speech_message(epoch_, capture_id_, "cancel"));
                o.note = text.empty() ? "No clear word heard. Hold Z and try again."
                                      : "'" + text + "' is not in the current vocabulary.";
            } else {
                o.messages.push_back(speech_message(epoch_, capture_id_, "result",
                                                    e.duration_ms ? e.duration_ms : 750, -1,
                                                    it->second));
                o.note = "Heard: " + text + ". Release Z when finished.";
            }
            break;
        }
        case SpeechEvent::Cancel:
            bound_ = false;
            o.messages.push_back(speech_message(epoch_, capture_id_, "cancel"));
            o.note = "No clear word heard. Hold Z and try again.";
            break;
        case SpeechEvent::Start:
            break;
    }
    return o;
}

SpeechMachine::Outcome SpeechMachine::abandon() {
    Outcome o;
    if (!bound_) return o;
    bound_ = false;
    o.messages.push_back(speech_message(epoch_, capture_id_, "cancel"));
    return o;
}

} // namespace retcomm::hub::vru
