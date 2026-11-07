#include "hub/hub_replay_info.hpp"
#include "hub/hub_states.hpp"
#include "core_link.hpp"
#include "state_envelope.hpp"
#include "retcomm/hash.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
using namespace retcomm::hub;
using namespace retro::corelink;
void check(bool ok,const char* why) { if (!ok) throw std::runtime_error(why); }
void write(const fs::path& p,const std::string& data) { std::ofstream f(p,std::ios::binary); f.write(data.data(),data.size()); }
std::string read(const fs::path& p) { std::ifstream f(p,std::ios::binary); return std::string(std::istreambuf_iterator<char>(f),{}); }
int main(int argc,char** argv) try {
    check(argc==4,"out, runner and core arguments required");
    const auto dir=utf8_path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir); write(dir/"content.txt","test");
    LaunchSpec spec; spec.runner=utf8_path(argv[2]); spec.core=utf8_path(argv[3]);
    spec.rom=path_utf8(dir/"content.txt"); spec.title_dir=dir; spec.session_dir=dir/"session"; spec.gl=false;
    CoreLink link; std::string error; const bool started=link.start(spec,&error); check(started,error.c_str());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    auto wait=[&](auto ready) {
        while (!ready() && link.state()!=LinkState::Ended && std::chrono::steady_clock::now()<deadline) link.pump(50);
        check(ready(),"link did not answer before deadline");
    };
    wait([&]{return link.state()==LinkState::Ready;});
    rcore_pad pads[RCORE_MAX_SEATS]{}; for (auto& pad:pads) pad.struct_size=sizeof pad;
    auto step=[&] {
        check(link.grant(pads),"grant failed"); const auto n=link.frames_granted();
        wait([&]{return link.frames_done()==n;}); link.take_frame();
    };
    for(int i=0;i<3;++i) step();
    const auto* frame=link.frame_info(); check(frame && frame->width && link.frame_pixels(),"no actual core picture");
    ReplayInfo captured; captured.path=dir/"record.route"; captured.title="Replay \"proof\"";
    captured.saved_unix=1700000000; captured.rate_num=60; captured.rate_den=1; captured.aspect=16.f/9.f;
    captured.pixels=retro::state::make_thumbnail(link.frame_pixels(),frame->width,frame->height,frame->stride);
    check(captured.pixels.size()==160*120*4 && captured.pixels.front()==3,"preview did not come from the displayed frame");
    check(link.request_recording(true,captured.path),"record start not sent");
    wait([&]{return !link.replay_pending();}); check(link.replay_status().state==ReplayState::Recording,"record start refused");
    for(int i=0;i<5;++i) step();
    check(link.request_recording(false),"record stop not sent"); wait([&]{return !link.replay_pending();});
    check(link.replay_status().state==ReplayState::Saved,"record stop refused"); link.stop();
    const auto movie_sha=retcomm::file_sha256_hex(captured.path);
    check(write_replay_preview(captured,error),"preview write failed");
    const auto original=read(replay_preview_path(captured.path));
    auto saved=read_replay_info(captured.path);
    check(saved.sealed && saved.fields==5 && saved.title==captured.title && saved.pixels==captured.pixels,"preview/metadata round trip failed");
    check(saved.aspect>1.77f && saved.aspect<1.78f && saved.saved_unix==captured.saved_unix,"aspect or date lost");
    check(!write_replay_preview(captured,error) && read(replay_preview_path(captured.path))==original,"existing preview overwritten");
    auto corrupt=original; corrupt.back()^=1; write(replay_preview_path(captured.path),corrupt);
    saved=read_replay_info(captured.path);
    check(saved.pixels.empty() && saved.preview_status=="Preview unavailable" && saved.fields==5,"corrupt preview was trusted");
    write(replay_preview_path(captured.path),std::string(90000,'x'));
    check(read_replay_info(captured.path).pixels.empty(),"oversized preview accepted");
    write(replay_preview_path(captured.path),original);
    const auto movie=read(captured.path);
    for(int i=0;i<17;++i) write(dir/("older-"+std::to_string(i)+".route"),movie);
    auto old=read_replay_info(dir/"older-0.route");
    check(old.sealed && old.pixels.empty() && old.preview_status=="No preview","legacy movie lost without preview");
    check(list_replays(dir).size()==18,"history was limited to twelve slots");
    write(dir/"older-0.route.preview",original);
    auto changed=movie; const auto at=changed.rfind("END "); changed[at+6]=changed[at+6]=='a' ? 'b' : 'a';
    write(dir/"older-0.route",changed);
    check(read_replay_info(dir/"older-0.route").pixels.empty(),"preview bound to another movie accepted");
    check(retcomm::file_sha256_hex(captured.path)==movie_sha,"metadata changed the recording");
    check(std::string(play_menu_page_title(PlayMenuPage::States))=="Save states" &&
          std::string(play_menu_page_title(PlayMenuPage::Replays))=="Replays","page headings ambiguous");
    check(toggle_browser_page(PlayMenuPage::States)==PlayMenuPage::Replays &&
          toggle_browser_page(PlayMenuPage::Replays)==PlayMenuPage::States &&
          toggle_browser_page(PlayMenuPage::Main)==PlayMenuPage::Main,"browser page switching changed");
    std::cout<<"hub_replays: actual frame preview, metadata protection, fallback, unlimited history and page navigation pass\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
