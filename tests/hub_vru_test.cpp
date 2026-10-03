// hub_vru: the VRU Microphone's windowless half -- the title's vocabulary,
// the core's dictionary, phrase matching and normalisation, the speech state
// machine against scripted dictionary / recognizer sequences, and the link's
// accessory message layout. Run as
// `retro-hub-vru-test <fixture vocabulary> <scratch dir>`.
//
// The dictionary lines below follow n64lle docs/VRU.md's `vru` object; the
// slots and sounds are the fixture's, not Hey You, Pikachu!'s.

#include "hub/hub_vru.hpp"

#if defined(RETCOMM_LINK_ACCESSORY_DATA)
#include "link_io.hpp" // Retro-Runtime corelink: as_accessory_msg
#endif

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace retcomm::hub::vru;
using json = nlohmann::json;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

void write(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

// A dictionary line the core would send: epoch, listening, capture_id and
// the fixture's words with their enabled flags.
std::string dict_line(std::uint64_t epoch, bool listening, std::uint64_t capture,
                      std::initializer_list<std::pair<const char*, bool>> words) {
    json d = {{"attached", true}, {"language", "en-US"}, {"epoch", epoch},
              {"listening", listening}, {"mode", 1}, {"pending", false},
              {"expected", 0}, {"capture_id", capture}, {"speech", false}};
    json ws = json::array();
    std::uint32_t slot = 0;
    for (const auto& [sounds, enabled] : words)
        ws.push_back({{"slot", slot++}, {"enabled", enabled}, {"sounds", sounds}});
    d["words"] = ws;
    return d.dump() + "\n";
}

void test_normalize() {
    check(normalize_phrase("  Good   Morning! ") == "good morning", "words lower-cased, one space");
    check(normalize_phrase("Good-bye!") == "good bye", "punctuation splits words");
    check(normalize_phrase("PIKACHU") == "pikachu", "upper-case");
    check(normalize_phrase("...") == "", "nothing left is empty");
}

void test_vocabulary(const fs::path& fixture, const fs::path& dir) {
    Vocabulary v;
    std::string err;
    check(load_vocabulary(fixture, v, &err), ("fixture loads: " + err).c_str());
    check(v.entries.size() == 6, "six entries");
    const auto pika = v.names("0A1B2C3D");
    check(pika.size() == 2 && pika[0] == "pika" && pika[1] == "pikachu",
          "two names for one sound, sorted, hex case ignored");
    check(v.names("3355") == std::vector<std::string>{"good bye"}, "normalized text");
    check(v.names("ffff").empty(), "an unknown sound names nothing");
    check(v.all_names().size() == 6 && v.all_names()[0] == "good bye", "every phrase, sorted");

    Vocabulary bad;
    check(!parse_vocabulary(R"({"schema":2,"language":"en-US","entries":[]})", bad, &err) &&
              err.find("schema-1") != std::string::npos,
          "another schema refused");
    check(!parse_vocabulary("not json", bad, &err), "not JSON refused");
    check(!load_vocabulary(dir / "missing.json", bad, &err) && err == "not found",
          "a missing file is 'not found'");

    // Where a title keeps it: game.toml's [vru] vocabulary, else the default name.
    const fs::path title = dir / "title";
    write(title / "game.toml", "[game]\nname = \"x\"\n\n[vru]\nvocabulary = \"words/en.json\"\n");
    check(vocabulary_path(title) == title / "words/en.json", "game.toml names the file");
    write(title / "game.toml", "[game]\nname = \"x\"\n");
    check(vocabulary_path(title) == title / "vru_vocabulary.json", "the default beside game.toml");
    check(vocabulary_path(dir / "none") == dir / "none" / "vru_vocabulary.json",
          "no game.toml: the default");
}

void test_dictionary(const Vocabulary& v) {
    Dictionary d;
    std::string err;
    check(parse_dictionary(dict_line(5, true, 9, {{"0a1b2c3d", true}, {"1122", false}, {"3344", true}}),
                           d, &err),
          "a dictionary line parses");
    check(d.attached && d.epoch == 5 && d.listening && d.capture_id == 9 && d.words.size() == 3,
          "fields read");
    check(d.words[1].slot == 1 && !d.words[1].enabled, "a disabled word");
    // The debug endpoint's wrapper, and numbers for flags, are taken too.
    check(parse_dictionary(R"({"ok":true,"id":3,"vru":{"attached":1,"epoch":2,"listening":0,"words":[]}})", d, &err) &&
              d.attached && d.epoch == 2 && !d.listening,
          "the vru wrapper is unwrapped, numbers are flags");
    check(!parse_dictionary("[1,2]", d, &err), "not an object refused");

    parse_dictionary(dict_line(5, true, 9, {{"0a1b2c3d", true}, {"1122", false}, {"3344", true}, {"ffff", true}}),
                     d, &err);
    const auto active = active_phrases(d, v);
    check(active.size() == 3, "enabled words the vocabulary knows");
    check(active.count("pika") && active.count("pikachu") && active.at("pikachu") == 0,
          "both names of a sound, at its slot");
    check(active.at("good morning") == 2 && !active.count("hello"), "disabled and unknown left out");
}

// The companion's LiveSpeech (tools/vru_client.py), as the hub runs it.
void test_machine(const Vocabulary& v) {
    SpeechMachine m;
    m.set_vocabulary(&v);
    auto ev = [](SpeechEvent::Kind k, std::uint64_t utt, const char* text = "",
                 float conf = 1.f, std::uint32_t ms = 0, int level = 0) {
        SpeechEvent e;
        e.kind = k;
        e.utterance = utt;
        e.text = text;
        e.confidence = conf;
        e.duration_ms = ms;
        e.level = level;
        return e;
    };
    auto parse = [](const std::string& line) { return json::parse(line); };

    // 1. No dictionary yet: speech is ambient.
    check(m.event(ev(SpeechEvent::Start, 1)).messages.empty() && !m.bound(), "no dictionary: nothing");
    check(m.active_names().empty(), "no names without a dictionary");

    // 2. The guest is not listening: ambient too.
    Dictionary d;
    std::string err;
    parse_dictionary(dict_line(3, false, 0, {{"0a1b2c3d", true}, {"1122", true}, {"3344", false}}), d, &err);
    m.dictionary(d);
    check(m.active_names() == std::vector<std::string>{"hello", "pika", "pikachu"},
          "the grammar is the enabled, known phrases, sorted");
    check(m.event(ev(SpeechEvent::Start, 2)).messages.empty() && !m.bound(), "not listening: nothing");
    check(m.event(ev(SpeechEvent::Final, 2, "pikachu")).messages.empty(), "its final: nothing");

    // 3. Listening: an onset binds to the capture; progress; a known final.
    parse_dictionary(dict_line(3, true, 7, {{"0a1b2c3d", true}, {"1122", true}, {"3344", false}}), d, &err);
    m.dictionary(d);
    SpeechMachine::Outcome o = m.event(ev(SpeechEvent::Start, 3));
    check(o.messages.size() == 1 && m.bound(), "an onset while listening binds");
    {
        const json j = parse(o.messages[0]);
        check(j["cmd"] == "vru_speech" && j["event"] == "start" && j["epoch"] == 3 &&
                  j["capture_id"] == 7 && !j.contains("id"),
              "start carries the epoch and capture, no transport id");
        check(o.messages[0].back() == '\n', "newline-terminated");
        check(o.note.find("Keep Z held") != std::string::npos, "the player is told");
    }
    o = m.event(ev(SpeechEvent::Progress, 3, "", 1.f, 240, 55));
    {
        const json j = parse(o.messages.at(0));
        check(j["event"] == "progress" && j["duration_ms"] == 240 && j["level"] == 55 &&
                  j["capture_id"] == 7,
              "progress carries duration and level");
    }
    check(m.event(ev(SpeechEvent::Progress, 9, "", 1.f, 100, 10)).messages.empty(),
          "another utterance's progress is ignored");
    o = m.event(ev(SpeechEvent::Final, 3, "Pikachu", 0.9f, 800));
    {
        const json j = parse(o.messages.at(0));
        check(j["event"] == "result" && j["slot"] == 0 && j["duration_ms"] == 800, "a known phrase: result with its slot");
        check(!m.bound(), "a final unbinds");
        check(o.note.find("Heard: pikachu") != std::string::npos, "the note names it");
    }

    // 4. Unknown text, low confidence, and a recognizer cancel each cancel.
    m.event(ev(SpeechEvent::Start, 4));
    o = m.event(ev(SpeechEvent::Final, 4, "good morning", 0.9f, 500));
    check(parse(o.messages.at(0))["event"] == "cancel" && o.note.find("not in the current vocabulary") != std::string::npos,
          "a disabled phrase cancels, saying so");
    m.event(ev(SpeechEvent::Start, 5));
    o = m.event(ev(SpeechEvent::Final, 5, "pikachu", 0.3f, 500));
    check(parse(o.messages.at(0))["event"] == "cancel" && o.note.find("unclear") != std::string::npos,
          "below 45% confidence cancels");
    m.event(ev(SpeechEvent::Start, 6));
    o = m.event(ev(SpeechEvent::Cancel, 6));
    check(parse(o.messages.at(0))["event"] == "cancel" && !m.bound(), "a recognizer cancel cancels");
    m.event(ev(SpeechEvent::Start, 7));
    o = m.event(ev(SpeechEvent::Final, 7, "", 1.f, 500));
    check(parse(o.messages.at(0))["event"] == "cancel", "an empty final cancels");

    // 5. A new epoch while bound: the capture is stale, nothing more is sent.
    m.event(ev(SpeechEvent::Start, 8));
    check(m.bound(), "bound again");
    parse_dictionary(dict_line(4, true, 11, {{"3344", true}}), d, &err);
    m.dictionary(d);
    check(!m.bound(), "a new epoch drops the capture");
    check(m.event(ev(SpeechEvent::Final, 8, "pikachu")).messages.empty(), "its final goes nowhere");
    check(m.active_names() == std::vector<std::string>{"good morning"}, "the grammar follows the epoch");
    // The slots are the new dictionary's.
    m.event(ev(SpeechEvent::Start, 9));
    o = m.event(ev(SpeechEvent::Final, 9, "GOOD  morning", 0.8f, 600));
    {
        const json j = parse(o.messages.at(0));
        check(j["event"] == "result" && j["slot"] == 0 && j["epoch"] == 4 && j["capture_id"] == 11,
              "a result under the new epoch, normalized text");
    }

    // 6. abandon: the mic stopped mid-capture.
    m.event(ev(SpeechEvent::Start, 10));
    o = m.abandon();
    check(parse(o.messages.at(0))["event"] == "cancel" && !m.bound(), "abandon cancels");
    check(m.abandon().messages.empty(), "abandon with nothing bound sends nothing");

    // 7. A dictionary that merely changes listening keeps the epoch's capture.
    m.event(ev(SpeechEvent::Start, 11));
    parse_dictionary(dict_line(4, false, 11, {{"3344", true}}), d, &err);
    m.dictionary(d);
    check(m.bound(), "same epoch: still bound");
    check(!m.event(ev(SpeechEvent::Final, 11, "good morning")).messages.empty(), "its final still goes out");

    const json sub = parse(SpeechMachine::submit_message(4, 2, 750));
    check(sub["cmd"] == "vru_submit" && sub["epoch"] == 4 && sub["slot"] == 2 && sub["duration_ms"] == 750,
          "vru_submit's shape");
}

#if defined(RETCOMM_LINK_ACCESSORY_DATA)
// The accessory message as the runner lays it out: header fields, then the
// bytes at their own length.
void test_link_message() {
    namespace cl = retro::corelink;
    static_assert(static_cast<std::uint32_t>(cl::Msg::AccessoryData) == 69, "AccessoryData = 69");
    static_assert(static_cast<std::uint32_t>(cl::Msg::AccessoryNotify) == 9, "AccessoryNotify = 9");
    static_assert(cl::kAccessoryMsgHeaderSize == 24, "seat, slot, len, pad after the header");
    static_assert(cl::kProtocolMajor == 2 && cl::kProtocolMinor >= 1, "link 2.1");
    const std::string body = "{\"cmd\":\"vru_speech\",\"event\":\"start\"}\n";
    cl::AccessoryDataMsg head{};
    head.h.type = cl::Msg::AccessoryNotify;
    head.h.size = static_cast<std::uint32_t>(cl::kAccessoryMsgHeaderSize + body.size());
    head.seat = 3;
    head.slot = 0;
    head.len = static_cast<std::uint32_t>(body.size());
    std::vector<unsigned char> pkt(cl::kAccessoryMsgHeaderSize + body.size());
    std::memcpy(pkt.data(), &head, cl::kAccessoryMsgHeaderSize);
    std::memcpy(pkt.data() + cl::kAccessoryMsgHeaderSize, body.data(), body.size());
    std::uint32_t seat = 0, slot = 9;
    std::vector<std::uint8_t> bytes;
    check(cl::as_accessory_msg(pkt, seat, slot, bytes) && seat == 3 && slot == 0 &&
              std::string(bytes.begin(), bytes.end()) == body,
          "a notify packet decodes to its seat, slot and bytes");
    pkt.resize(pkt.size() - 4);
    check(!cl::as_accessory_msg(pkt, seat, slot, bytes), "a packet shorter than its len is refused");
    pkt.resize(8);
    check(!cl::as_accessory_msg(pkt, seat, slot, bytes), "a packet without the fields is refused");
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <fixture vocabulary.json> <scratch dir>\n", argv[0]);
        return 2;
    }
    const fs::path fixture = argv[1];
    const fs::path dir = argv[2];
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    test_normalize();
    test_vocabulary(fixture, dir);
    Vocabulary v;
    std::string err;
    if (!load_vocabulary(fixture, v, &err)) {
        std::fprintf(stderr, "fixture %s: %s\n", fixture.string().c_str(), err.c_str());
        return 1;
    }
    test_dictionary(v);
    test_machine(v);
#if defined(RETCOMM_LINK_ACCESSORY_DATA)
    test_link_message();
#else
    std::fprintf(stderr, "note: built without the link's accessory data; its message test is skipped\n");
#endif
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("hub_vru: all checks passed\n");
    return 0;
}
