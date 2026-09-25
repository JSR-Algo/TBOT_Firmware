#include "lesson_layered_cinematic_renderer.h"
#include "lesson_mjpeg_mp4.h"
#include "jpeg_to_image.h"
#define LODEPNG_NO_COMPILE_CPP
#include "lodepng.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

using namespace tbot;
namespace {
std::size_t checks = 0;
void Check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL " << message << std::endl; std::exit(1); }
}
std::vector<std::uint8_t> Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.good(), "open " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Asset {
    std::string kind, name, path;
    unsigned width, height, fps, frames, duration;
    LessonCinematicRect rect;
};
struct Stream {
    std::vector<std::uint8_t> bytes;
    LessonMjpegMp4Reader reader;
    jpeg_reusable_decoder_t jpeg{};
};
struct Context {
    std::string output, name;
    std::map<void*, std::size_t> allocations;
    std::size_t live = 0, peak = 0, opens = 0, closes = 0;
    std::size_t jpeg_calls = 0, png_calls = 0, decoded = 0, presented = 0;
    std::size_t png_partial = 0, png_transparent = 0;
    unsigned frame_count = 0;
    std::vector<std::uint16_t> background, static_pixels, last;
    std::vector<std::uint8_t> object;
    std::set<std::size_t> seen;
    std::set<std::uint64_t> fingerprints;
    std::vector<double> decode_ms;
    LessonCinematicRect robot_rect{}, object_rect{};
    std::size_t robot_nonkey_pixels = 0, robot_key_pixels = 0, pixel_checks = 0;
};
std::uint16_t Pack(unsigned r, unsigned g, unsigned b) {
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
std::array<unsigned, 3> Rgb(unsigned p) {
    return {((p >> 11) << 3) | ((p >> 11) >> 2),
            (((p >> 5) & 63) << 2) | (((p >> 5) & 63) >> 4),
            ((p & 31) << 3) | ((p & 31) >> 2)};
}
void MakeStatic(Context& c) {
    if (c.background.empty() || c.object.empty()) return;
    c.static_pixels = c.background;
    const auto rect = c.object_rect;
    for (unsigned y = 0; y < rect.height; ++y) {
        for (unsigned x = 0; x < rect.width; ++x) {
            const auto* rgba = c.object.data() + (y * rect.width + x) * 4;
            auto& destination = c.static_pixels[(y + rect.y) * 480 + x + rect.x];
            const auto back = Rgb(destination);
            const unsigned alpha = rgba[3];
            destination = Pack((rgba[0] * alpha + back[0] * (255-alpha) + 127)/255,
                               (rgba[1] * alpha + back[1] * (255-alpha) + 127)/255,
                               (rgba[2] * alpha + back[2] * (255-alpha) + 127)/255);
        }
    }
}
void* Allocate(void* opaque, std::size_t bytes) {
    auto& c = *static_cast<Context*>(opaque);
    auto* pointer = std::malloc(bytes);
    if (pointer) { c.allocations[pointer] = bytes; c.live += bytes; c.peak = std::max(c.peak,c.live); }
    return pointer;
}
void Free(void* opaque, void* pointer) {
    auto& c = *static_cast<Context*>(opaque);
    Check(c.allocations.count(pointer) == 1, "renderer frees owned allocation once");
    c.live -= c.allocations.at(pointer); c.allocations.erase(pointer); std::free(pointer);
}
bool DecodeJpeg(void* opaque, const char* path, std::uint16_t* out, std::size_t capacity,
                std::uint16_t* width, std::uint16_t* height, std::size_t* stride) {
    auto& c = *static_cast<Context*>(opaque);
    const auto bytes = Read(path);
    std::uint8_t* decoded = nullptr;
    std::size_t size = 0, w = 0, h = 0, s = 0;
    const auto error = jpeg_to_image_with_caps(bytes.data(), bytes.size(), &decoded, &size,
                                              &w, &h, &s, 0x1000);
    Check(error == ESP_OK, "actual background JPEG decodes");
    Check(w == 480 && h == 320 && size == 307200 && s == 960 && capacity >= size,
          "background RGB565 dimensions and bounds");
    std::memcpy(out, decoded, size); std::free(decoded);
    *width = w; *height = h; *stride = s / 2;
    c.background.assign(out, out + w * h); ++c.jpeg_calls; MakeStatic(c);
    return true;
}
bool DecodePng(void* opaque, const char* path, std::uint8_t* out, std::size_t capacity,
               std::uint16_t* width, std::uint16_t* height, std::size_t* stride) {
    auto& c = *static_cast<Context*>(opaque);
    const auto bytes = Read(path);
    std::uint8_t* decoded = nullptr; unsigned w = 0, h = 0;
    Check(lodepng_decode32(&decoded, &w, &h, bytes.data(), bytes.size()) == 0,
          "actual PNG decodes with bundled LVGL LodePNG");
    auto* draw = reinterpret_cast<lv_draw_buf_t*>(decoded);
    Check(w == 95 && h == 95 && draw && draw->data_size == w*h*4 && capacity >= draw->data_size,
          "object RGBA dimensions and bounds");
    // LodePNG produces RGBA. LVGL's BGRA conversion and handler's inverse cancel.
    std::memcpy(out, draw->data, draw->data_size);
    c.object.assign(out, out + draw->data_size);
    for (std::size_t i = 3; i < c.object.size(); i += 4) {
        if (c.object[i] == 0) ++c.png_transparent;
        else if (c.object[i] != 255) ++c.png_partial;
    }
    *width = w; *height = h; *stride = w*4;
    lv_draw_buf_destroy(draw); ++c.png_calls; MakeStatic(c);
    return true;
}
bool ReadAt(void* opaque, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    auto& bytes = static_cast<Stream*>(opaque)->bytes;
    if (offset > bytes.size() || size > bytes.size() - offset) return false;
    std::memcpy(out, bytes.data() + offset, size); return true;
}
bool Open(void* opaque, const char* path, LessonCinematicStreamMetadata* metadata, void** handle) {
    auto& c = *static_cast<Context*>(opaque);
    auto* stream = new Stream; stream->bytes = Read(path);
    Check(stream->reader.Open({stream, ReadAt, stream->bytes.size()}) == LessonMjpegMp4Status::kOk,
          "actual supplied MP4 parses");
    auto& r = stream->reader;
    std::uint32_t largest = 0;
    for (std::size_t i = 0; i < r.frame_count(); ++i) {
        LessonMjpegMp4Frame f{};
        Check(r.GetFrame(i, &f) == LessonMjpegMp4Status::kOk, "actual sample table address");
        largest = std::max(largest,f.size);
    }
    *metadata = {r.width(),r.height(),static_cast<std::uint16_t>(r.fps_milli()/1000),
                 static_cast<std::uint32_t>(r.frame_count()),
                 static_cast<std::uint32_t>(r.duration_ticks()*1000/r.timescale()),largest};
    Check(jpeg_reusable_decoder_prepare_workspace(&stream->jpeg,240,240,0x1000) == ESP_OK,
          "production reusable JPEG workspace initializes");
    *handle = stream; ++c.opens; return true;
}
void Close(void* opaque, void* handle) {
    auto& c = *static_cast<Context*>(opaque);
    auto* stream = static_cast<Stream*>(handle);
    jpeg_reusable_decoder_destroy(&stream->jpeg); delete stream; ++c.closes;
}
bool DecodeVideo(void* opaque, void* handle, std::size_t frame, std::uint8_t* out,
                 std::size_t capacity, std::uint16_t* width, std::uint16_t* height,
                 std::size_t* stride) {
    auto& c = *static_cast<Context*>(opaque); auto& stream = *static_cast<Stream*>(handle);
    LessonMjpegMp4Frame f{};
    Check(stream.reader.GetFrame(frame,&f) == LessonMjpegMp4Status::kOk, "frame index admitted");
    std::vector<std::uint8_t> sample(f.size); std::size_t read = 0;
    Check(stream.reader.ReadFrame(frame,sample.data(),sample.size(),&read) == LessonMjpegMp4Status::kOk
          && read == sample.size(), "actual JPEG sample reads exactly");
    std::size_t size=0,w=0,h=0,s=0;
    const auto started = std::chrono::steady_clock::now();
    const auto error = jpeg_reusable_decoder_decode_into(&stream.jpeg,sample.data(),sample.size(),
                                                        out,capacity,&size,&w,&h,&s);
    c.decode_ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
    if (error != ESP_OK) {
        std::cerr << "DECODE_FAILURE " << c.name << " frame=" << frame << " esp_error=" << error << '\n';
        return false;
    }
    Check(w == stream.reader.width() && h == stream.reader.height() && s == w*2 && size == w*h*2,
          "decoded frame dimensions and stride match actual MP4");
    *width=w; *height=h; *stride=s;
    ++c.decoded; c.seen.insert(frame);
    return true;
}
std::uint64_t Fingerprint(const std::uint16_t* pixels,std::size_t size) {
    std::uint64_t value=1469598103934665603ULL;
    for (std::size_t i=0;i<size;++i) { value^=pixels[i]; value*=1099511628211ULL; }
    return value;
}
bool Present(void* opaque,const std::uint16_t* pixels,std::uint16_t width,std::uint16_t height,
              std::size_t frame) {
    auto& c = *static_cast<Context*>(opaque);
    Check(width == 480 && height == 320 && c.static_pixels.size() == 480*320,
          "actual 480x320 framebuffer reaches host present");
    const auto r = c.robot_rect;
    std::size_t mismatch = 0;
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        if (static_cast<int>(x)>=r.x && static_cast<int>(x)<r.x+r.width &&
            static_cast<int>(y)>=r.y && static_cast<int>(y)<r.y+r.height) continue;
        ++c.pixel_checks;
        if (pixels[y*width+x] != c.static_pixels[y*width+x]) ++mismatch;
    }
    Check(mismatch == 0, "background and RGBA object pixels persist outside robot rect");
    c.last.assign(pixels,pixels+width*height);
    c.fingerprints.insert(Fingerprint(pixels,width*height));
    ++c.presented;
    if (frame == 0 || frame == c.frame_count/2 || frame+1 == c.frame_count) {
        const auto filename = c.output+"/"+c.name+"-f"+std::to_string(frame)+".ppm";
        if (!std::filesystem::exists(filename)) {
            std::ofstream file(filename,std::ios::binary);
            file << "P6\n480 320\n255\n";
            for (std::size_t i=0;i<width*height;++i) {
                const auto rgb=Rgb(pixels[i]);
                for (auto channel:rgb) file.put(static_cast<char>(channel));
            }
            Check(file.good(), "capture host RGB pixels");
        }
    }
    return true;
}
std::uint64_t Clock(void*) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace
extern "C" void* heap_caps_malloc(std::size_t bytes,std::uint32_t) { return std::malloc(bytes); }
extern "C" void* heap_caps_aligned_calloc(std::size_t alignment,std::size_t count,std::size_t bytes,std::uint32_t) {
    void* p=nullptr; if (posix_memalign(&p,alignment,count*bytes)) return nullptr;
    std::memset(p,0,count*bytes); return p;
}
extern "C" void heap_caps_free(void* p) { std::free(p); }

int main(int argc,char** argv) {
    Check(argc == 4,"input manifest, output and scope arguments");
    const std::string mode=argv[3];
    Check(mode=="full" || mode=="static" || mode=="inventory","explicit known verification scope");
    std::ifstream input(argv[1]); std::vector<Asset> images,videos,selected; std::string line;
    while (std::getline(input,line)) {
        std::istringstream row(line); Asset a{};
        std::getline(row,a.kind,'\t'); std::getline(row,a.name,'\t'); std::getline(row,a.path,'\t');
        row >> a.width >> a.height >> a.rect.x >> a.rect.y >> a.rect.width >> a.rect.height
            >> a.fps >> a.frames >> a.duration;
        Check(!row.fail(),"parse pinned media row");
        (a.kind=="image" ? images : a.kind=="video" ? videos : selected).push_back(a);
    }
    Check(images.size()==4 && videos.size()==6,"four images and six distinct actual clips");
    Context c{}; c.output=argv[2]; c.object_rect=images[1].rect;
    if (mode=="static") {
        std::vector<std::uint16_t> background(480*320);
        std::vector<std::uint8_t> object(240*240*4);
        std::uint16_t w=0,h=0; std::size_t stride=0;
        Check(DecodeJpeg(&c,images[0].path.c_str(),background.data(),background.size()*2,&w,&h,&stride),
              "independent actual JPEG");
        for (std::size_t i=1;i<images.size();++i) {
            Check(DecodePng(&c,images[i].path.c_str(),object.data(),object.size(),&w,&h,&stride),
                  "independent actual PNG");
            c.name=images[i].name+"-static"; c.frame_count=1;
            Check(Present(&c,c.static_pixels.data(),480,320,0),"independent static host capture");
        }
        Check(c.png_partial>0 && c.png_transparent>0,"real PNG has partial and transparent alpha");
        std::ofstream results(c.output+"/static-results.json");
        results<<"{\"jpegPass\":1,\"pngPass\":3,\"failed\":0,\"skipped\":0,\"checks\":"<<checks
               <<",\"partialAlphaPixels\":"<<c.png_partial<<",\"transparentAlphaPixels\":"<<c.png_transparent
               <<",\"animationQualified\":false,\"scope\":\"production JPEG and bundled LodePNG decode; test alpha reference composition only\"}\n";
        std::cout<<"PASS independent static decoding: 4 cases, 0 failed, 0 skipped; "<<checks<<" checks; animation remains unqualified\n";
        return 0;
    }
    if (mode=="inventory") {
        std::ofstream results(c.output+"/decode-inventory.jsonl");
        std::size_t total=0,success=0;
        for (const auto* corpus : {&videos,&selected}) for (const auto& a:*corpus) {
            c.name=a.name; void* stream=nullptr; LessonCinematicStreamMetadata metadata{};
            Check(Open(&c,a.path.c_str(),&metadata,&stream),"inventory stream opens");
            Check(metadata.width==a.width && metadata.height==a.height && metadata.frame_count==a.frames
                  && metadata.fps==a.fps && metadata.duration_ms==a.duration,"pinned actual video metadata matches parser");
            std::vector<std::uint8_t> frame(240*240*2); std::size_t passed=0;
            for (unsigned index=0;index<a.frames;++index) {
                std::uint16_t w=0,h=0;std::size_t stride=0;
                if (DecodeVideo(&c,stream,index,frame.data(),frame.size(),&w,&h,&stride)) ++passed;
            }
            Close(&c,stream); total+=a.frames; success+=passed;
            results<<"{\"sourceSet\":\""<<(corpus==&videos?"currentT09":"selectedT07")<<"\",\"phase\":\""<<a.name
                   <<"\",\"collected\":"<<a.frames<<",\"pass\":"<<passed<<",\"fail\":"<<a.frames-passed
                   <<",\"skip\":0}\n";
        }
        results.flush();
        std::cout<<"Actual decoder inventory: "<<total<<" collected, "<<success<<" pass, "<<total-success
                 <<" fail, 0 skip; selected listen/thinking overlap\n";
        Check(total==711,"all333 current and378 selected bindings attempted");
        return success==total ? 0 : 1;
    }
    LessonLayeredCinematicRenderer renderer({&c,Allocate,Free,DecodeJpeg,DecodePng,Open,Close,
                                             DecodeVideo,Present,nullptr,Clock});
    std::ofstream results(c.output+"/results.jsonl");
    std::size_t total_unique=0; std::uint64_t sequence=1;
    for (std::size_t clip=0;clip<videos.size();++clip) {
        const auto& a=videos[clip]; c.name=a.name; c.frame_count=a.frames; c.robot_rect=a.rect;
        c.seen.clear(); c.fingerprints.clear(); const auto before=c.presented;
        LessonLayeredCinematicPhaseConfig config{};
        config.renderer_id=kLessonRendererV5; config.template_id=kLessonLayeredCinematicTemplate;
        config.phase_id=a.name.c_str(); config.command_sequence_id=sequence++;
        config.fps=a.fps; config.frame_count=a.frames; config.duration_ms=a.duration;
        config.retain_static_layers=clip!=0;
        config.background={images[0].path.c_str(),images[0].rect,images[0].path.c_str()};
        config.teaching_object={images[1].path.c_str(),images[1].rect,images[1].path.c_str()};
        config.robot={a.path.c_str(),a.rect,{{0,255,0},32,4}};
        Check(renderer.Prepare(config,0).accepted && !renderer.last_apply_degraded(),"actual clip prepares without fallback");
        Check(renderer.Start(sequence++,a.name.c_str(),0).accepted,"actual clip starts");
        for (unsigned frame=1;frame<a.frames;++frame) {
            Check(renderer.Tick((frame*1000+a.fps-1)/a.fps).accepted,"scheduled actual frame advances");
        }
        Check(c.seen.size()==a.frames,"every supplied frame decoded");
        Check(c.presented-before==a.frames,"once renders each frame exactly once");
        Check(c.fingerprints.size()>3,"animation has distinct displayed host pixels");
        Check(renderer.Tick(a.duration).type==LessonCinematicResponseType::kPhaseComplete,"once completes after final frame");
        total_unique+=c.seen.size();
        Check(c.jpeg_calls==1 && c.png_calls==1,"replacement reuses actual static images");
        results << "{\"phase\":\""<<a.name<<"\",\"uniqueFrames\":"<<c.seen.size()
                <<",\"distinctCompositeFrames\":"<<c.fingerprints.size()<<",\"presented\":"<<c.presented-before<<"}\n";
    }
    const auto& a=videos[3]; c.name="listen-loop"; c.robot_rect=a.rect; c.frame_count=a.frames;
    LessonLayeredCinematicPhaseConfig loop{};
    loop.renderer_id=kLessonRendererV5; loop.template_id=kLessonLayeredCinematicTemplate;
    loop.phase_id="listen"; loop.command_sequence_id=sequence++; loop.fps=a.fps;
    loop.frame_count=a.frames; loop.duration_ms=a.duration; loop.retain_static_layers=true;
    loop.playback_mode=LessonLayeredPlaybackMode::kLoop;
    loop.background={images[0].path.c_str(),images[0].rect,images[0].path.c_str()};
    loop.teaching_object={images[1].path.c_str(),images[1].rect,images[1].path.c_str()};
    loop.robot={a.path.c_str(),a.rect,{{0,255,0},32,4}};
    Check(renderer.Prepare(loop,0).accepted,"loop prepares");
    Check(renderer.Start(sequence++,"listen",0).accepted,"loop starts");
    Check(renderer.Tick(1000).accepted,"loop reaches middle");
    Check(renderer.Pause(sequence++,"listen",1000).accepted,"pause accepted");
    auto presentations=c.presented; renderer.Tick(9000);
    Check(c.presented==presentations,"pause emits no content");
    Check(renderer.Resume(sequence++,"listen",9000).accepted,"resume accepted");
    Check(renderer.Tick(9067).accepted,"resume advances logical clock");
    Check(renderer.Tick(11000).accepted,"loop wraps");
    auto late_before=c.presented;
    Check(renderer.Tick(60000067).accepted && c.presented==late_before+1,"very late tick presents one frame without queued history");
    for (std::size_t i=2;i<images.size();++i) {
        loop.command_sequence_id=sequence++; loop.teaching_object={images[i].path.c_str(),images[i].rect,images[i].path.c_str()};
        c.name=images[i].name+"-replacement";
        Check(renderer.Prepare(loop,0).accepted && !renderer.last_apply_degraded(),"actual activity object replaces without fallback");
        Check(c.jpeg_calls==1 && c.png_calls==i,"object replacement decodes new identity and reuses background");
    }
    Check(c.png_partial>0 && c.png_transparent>0,"supplied objects exercise partial and transparent alpha");
    Check(renderer.Cancel(sequence++,"listen").accepted,"cancel actual media");
    presentations=c.presented; renderer.Tick(60001000);
    Check(c.presented==presentations,"cancel emits no further frame");
    renderer.DiscardSession();
    Check(c.live==0 && c.allocations.empty() && c.opens==c.closes,"all renderer buffers and streams released");
    Check(total_unique==333,"333 supplied unique source frames verified");
    std::sort(c.decode_ms.begin(),c.decode_ms.end());
    results << "{\"summary\":true,\"uniqueSourceFrames\":"<<total_unique<<",\"decoded\":"<<c.decoded
            <<",\"presented\":"<<c.presented<<",\"staticPixelComparisons\":"<<c.pixel_checks
            <<",\"rendererPeakBytes\":"<<c.peak<<",\"jpegCalls\":"<<c.jpeg_calls
            <<",\"pngCalls\":"<<c.png_calls<<",\"partialAlphaPixels\":"<<c.png_partial
            <<",\"transparentAlphaPixels\":"<<c.png_transparent
            <<",\"hostDecodeMedianMs\":"<<c.decode_ms[c.decode_ms.size()/2]
            <<",\"hostDecodeMaxMs\":"<<c.decode_ms.back()<<",\"checks\":"<<checks<<"}\n";
    std::cout<<"PASS actual media: 333 unique JPEG frames, 4 images, "<<c.presented
             <<" host presentations, "<<checks<<" checks, 0 failed, 0 skipped; ASan/UBSan enabled\n";
}
