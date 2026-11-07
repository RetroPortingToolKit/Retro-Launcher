#include "hub/hub_replay_info.hpp"
#include "retcomm/hash.hpp"
#include "state_envelope.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace retcomm::hub {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t kPixels = retro::state::kThumbWidth * retro::state::kThumbHeight * 4;
constexpr std::size_t kMaxPreview = kPixels + 4096;
constexpr const char* kMagic = "RETRO_REPLAY_PREVIEW 1";
bool footer(ReplayInfo& info) {
    std::ifstream in(info.path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto size=in.tellg();
    if (size<16) return false;
    const auto n=std::min<std::streamoff>(size,256);
    in.seekg(size-n);
    std::string text(static_cast<std::size_t>(n),'\0'); in.read(text.data(),n);
    const auto at=text.rfind("\nEND ");
    if (at==std::string::npos || text.back()!='\n') return false;
    std::istringstream row(text.substr(at+5)); std::string extra;
    if (!(row>>info.fields>>info.seal) || !info.fields || info.fields>26843545 || info.seal.size()!=64 ||
        info.seal.find_first_not_of("0123456789abcdef")!=std::string::npos || row>>extra) return false;
    return true; // Display hint only; the runner checks the complete movie.
}
}
fs::path replay_preview_path(const fs::path& movie) { auto p=movie; p += ".preview"; return p; }
ReplayInfo read_replay_info(const fs::path& movie) {
    ReplayInfo info; info.path=movie; info.title=movie.stem().u8string();
    info.sealed=footer(info); if (!info.sealed) { info.fields=0; info.seal.clear(); }
    info.preview_status=info.sealed ? "No preview" : "Replay incomplete";
    std::error_code ec;
    const auto modified=fs::last_write_time(movie,ec);
    if (!ec) info.saved_unix=std::chrono::duration_cast<std::chrono::seconds>(
        (modified-fs::file_time_type::clock::now()+std::chrono::system_clock::now()).time_since_epoch()).count();
    const auto path=replay_preview_path(movie);
    if (!fs::exists(path,ec)) return info;
    info.preview_status="Preview unavailable";
    const auto size=fs::file_size(path,ec);
    if (ec || size>kMaxPreview) return info;
    std::ifstream file(path,std::ios::binary);
    std::string data(static_cast<std::size_t>(size),'\0');
    if (!file.read(data.data(),static_cast<std::streamsize>(size)) || file.get()!=std::char_traits<char>::eof()) return info;
    const auto line=data.find('\n'), body=line==std::string::npos ? line : data.find('\n',line+1);
    if (line==std::string::npos || body==std::string::npos || data.substr(0,line)!=kMagic) return info;
    const auto payload=data.substr(body+1);
    if (data.substr(line+1,body-line-1)!=retcomm::sha256_hex(payload)) return info;
    std::istringstream in(payload); ReplayInfo saved; std::size_t bytes=0; std::string seal;
    if (!(in>>seal>>saved.saved_unix>>saved.rate_num>>saved.rate_den>>saved.aspect>>bytes>>std::quoted(saved.title)) ||
        seal!=info.seal || !info.sealed || !std::isfinite(saved.aspect) || saved.aspect<.1f || saved.aspect>10.f ||
        saved.title.size()>1024 || saved.saved_unix<0 ||
        (bytes!=0 && bytes!=kPixels) || (saved.rate_num && !saved.rate_den) || saved.rate_num>1000000 || saved.rate_den>1000000 ||
        in.get()!='\n') return info;
    const auto start=in.tellg();
    if (start<0 || payload.size()-static_cast<std::size_t>(start)!=bytes) return info;
    info.title=saved.title; info.saved_unix=saved.saved_unix;
    info.rate_num=saved.rate_num; info.rate_den=saved.rate_den; info.aspect=saved.aspect;
    info.pixels.assign(payload.begin()+static_cast<std::size_t>(start),payload.end());
    info.preview_status=info.pixels.empty() ? "No preview" : "";
    return info;
}
bool write_replay_preview(const ReplayInfo& info, std::string& error) {
    ReplayInfo movie; movie.path=info.path;
    if (!footer(movie)) { error="The replay is not sealed; preview was not saved"; return false; }
    if ((!info.pixels.empty() && info.pixels.size()!=kPixels) || info.title.size()>1024 ||
        !std::isfinite(info.aspect) || info.aspect<.1f || info.aspect>10.f || info.saved_unix<0 ||
        info.rate_num>1000000 || info.rate_den>1000000 || (info.rate_num && !info.rate_den)) {
        error="Invalid replay preview data"; return false;
    }
    const auto path=replay_preview_path(info.path);
    std::error_code ec;
    if (fs::exists(path,ec)) { error="A replay preview already exists"; return false; }
    if (ec) { error=ec.message(); return false; }
    std::ostringstream header;
    header<<movie.seal<<' '<<info.saved_unix<<' '<<info.rate_num<<' '<<info.rate_den<<' '
          <<info.aspect<<' '<<info.pixels.size()<<' '<<std::quoted(info.title)<<'\n';
    auto payload=header.str();
    if (!info.pixels.empty()) payload.append(reinterpret_cast<const char*>(info.pixels.data()),info.pixels.size());
    std::ofstream out(path,std::ios::binary);
    out<<kMagic<<'\n'<<retcomm::sha256_hex(payload)<<'\n'; out.write(payload.data(),payload.size()); out.close();
    if (!out) { error="Could not save replay preview"; return false; }
    return true;
}
std::string replay_duration(const ReplayInfo& info) {
    if (!info.rate_num || !info.rate_den) return std::to_string(info.fields)+" fields";
    const auto seconds=info.fields*std::uint64_t(info.rate_den)/info.rate_num;
    std::ostringstream out; out<<seconds/60<<':'<<std::setw(2)<<std::setfill('0')<<seconds%60;
    return out.str()+" ("+std::to_string(info.fields)+" fields)";
}
std::vector<ReplayInfo> list_replays(const fs::path& directory) {
    std::vector<ReplayInfo> result; std::error_code ec;
    for (fs::directory_iterator it(directory,ec),end; !ec && it!=end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension()==".route") result.push_back(read_replay_info(it->path()));
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) {
        return a.saved_unix!=b.saved_unix ? a.saved_unix>b.saved_unix : a.path>b.path;
    });
    return result;
}
} // namespace retcomm::hub
