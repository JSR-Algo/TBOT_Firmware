#include "lesson_original_source_session.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
size_t original_source_live(void);
size_t original_source_peak(void);
size_t original_source_calls(void);
size_t original_source_injections(void);
void original_source_fault_reset(size_t);
void original_source_fail_at(size_t);
void original_source_on_allocation(void (*)(size_t));
void original_source_bind(tbot::OriginalSourceAllocationState*);
}
using namespace tbot;
static OriginalSourceAllocationState allocation_state;
// Every Open runs under a real coordinator lesson-session read lease.
static const char* const kAssignment = "assignment-original";
static const char* const kSessionId = "session-original";
static uint64_t lesson_generation = 0;
static bool expect_lease_held = false, lease_held_checked = false;
static LessonAssetStorageCoordinator& Coordinator() { return LessonAssetStorageCoordinator::GetInstance(); }
static void BeginLesson() {
    const auto begun = Coordinator().TryBeginLessonSession(kAssignment, kSessionId);
    assert(begun.acquired);
    lesson_generation = begun.generation;
}
static LessonAssetReadLease Lease() {
    auto lease = Coordinator().TryRetainLessonSession(kAssignment, kSessionId, lesson_generation);
    assert(lease);
    return lease;
}
static std::atomic<bool>* cancel_target;
static size_t cancel_allocation, cancel_read, read_calls;
static int cancel_receive_code = 1;
static bool cancel_send_error = false;
static int mutate_packet = 0, mutate_frame = 0;
static unsigned seam_calls = 0, cancel_at = 0;
static AVCodecContext* first_codec = nullptr;
static bool hold_first_send = false, held_send = false;
static size_t synthetic_bytes = 0;
static unsigned synthetic_frames = 0, synthetic_emitted = 0;
static bool delay_output = false, flush_eagain = false;
static AVCodecContext* delayed_codec[2]{};
static AVFrame* delayed_frame[2]{};
static bool drain_attempted[2]{};
static int DelaySide(AVCodecContext* codec) {
    if(!delayed_codec[0])delayed_codec[0]=codec;
    if(delayed_codec[0]==codec)return 0;
    if(!delayed_codec[1])delayed_codec[1]=codec;
    assert(delayed_codec[1]==codec);return 1;
}
static void Seam() {
    if (cancel_target && cancel_at && ++seam_calls == cancel_at) cancel_target->store(true);
}
// Post-integrity demux seam exercises malformed metadata, not the SHA gate.
extern "C" int source_read_frame(AVFormatContext* format, AVPacket* packet) {
    int result=av_read_frame(format,packet);
    Seam();
    if(result>=0 && mutate_packet) {
        size_t size=0;
        uint8_t* data=av_packet_get_side_data(packet,AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL,&size);
        if(data) {
            if(mutate_packet==1) av_packet_free_side_data(packet);
            else if(mutate_packet==2 || mutate_packet==3) data[7]=mutate_packet==2?0:2;
            else if(mutate_packet==4 || mutate_packet==5) {
                for(int i=0;i<packet->side_data_elems;i++)
                    if(packet->side_data[i].type==AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL)
                        packet->side_data[i].size=mutate_packet==4?7:8;
            } else if(mutate_packet==6) memset(data+8,0,size-8);
        }
    }
    return result;
}
static void CancelAllocation(size_t index) {
    if (cancel_target && index==cancel_allocation) cancel_target->store(true);
}
extern "C" size_t source_fread(void* p, size_t size, size_t count, FILE* file) {
    if(expect_lease_held) {
        // An active reader blocks session end, so SD cannot be released mid-snapshot.
        assert(!Coordinator().EndLessonSession(kAssignment,kSessionId,lesson_generation));
        lease_held_checked=true;
    }
    size_t result=fread(p,size,count,file);
    if(cancel_target && ++read_calls==cancel_read)cancel_target->store(true);
    return result;
}

extern "C" int source_receive_frame(AVCodecContext* codec, AVFrame* frame) {
    if(delay_output) {
        int i=DelaySide(codec);
        if(delayed_frame[i] && drain_attempted[i]) {
            av_frame_move_ref(frame,delayed_frame[i]);av_frame_free(&delayed_frame[i]);
            return 0;
        }
        int result=avcodec_receive_frame(codec,frame);
        if(result==0) {
            // Keep the last real frame until drain; earlier retained output remains ordered.
            AVFrame* next=av_frame_alloc();assert(next);av_frame_move_ref(next,frame);
            if(delayed_frame[i]) {
                av_frame_move_ref(frame,delayed_frame[i]);av_frame_free(&delayed_frame[i]);
            } else result=AVERROR(EAGAIN);
            delayed_frame[i]=next;
        }
        return result;
    }
    if(synthetic_frames) {
        if(!first_codec)first_codec=codec;
        if(codec!=first_codec)return AVERROR(EAGAIN);
        if(synthetic_emitted==synthetic_frames) {
            assert(cancel_target);cancel_target->store(true);return AVERROR(EAGAIN);
        }
        av_frame_unref(frame);
        frame->buf[0]=av_buffer_alloc(synthetic_bytes);assert(frame->buf[0]);
        frame->data[0]=frame->buf[0]->data;
        frame->width=480;frame->height=480;frame->format=AV_PIX_FMT_YUV420P;
        frame->pts=synthetic_emitted++;
        return 0;
    }
    int result=avcodec_receive_frame(codec,frame);
    Seam();
    if(result==0 && mutate_frame) {
        if(mutate_frame==1) frame->pts=AV_NOPTS_VALUE;
        if(mutate_frame==2) frame->width++;
        if(mutate_frame==3) frame->format=AV_PIX_FMT_YUV420P10LE;
        if(mutate_frame==4) frame->pts=0;
    }
    if(cancel_target && result==cancel_receive_code)cancel_target->store(true);
    return result;
}
extern "C" int source_send_packet(AVCodecContext* codec, const AVPacket* packet) {
    Seam();
    if(delay_output && !packet) {
        int i=DelaySide(codec);
        if(!drain_attempted[i]) {
            drain_attempted[i]=true;
            if(flush_eagain)return AVERROR(EAGAIN);
        }
    }
    if(hold_first_send && !first_codec) first_codec=codec;
    if(hold_first_send && codec==first_codec && !held_send) {
        held_send=true;
        return AVERROR(EAGAIN);
    }
    int result=avcodec_send_packet(codec,packet);
    if(cancel_target && cancel_send_error) {
        cancel_target->store(true);
        return AVERROR_INVALIDDATA;
    }
    return result;
}
static std::string FrameRow(const OriginalSourceFrame& f,unsigned index) {
    alignas(16) uint8_t state[256];assert(static_cast<size_t>(av_md5_size)<=sizeof(state));
    auto* md5=reinterpret_cast<AVMD5*>(state);av_md5_init(md5);
    for(int p=0;p<(f.rgba?1:(f.planes[3]?4:3));p++) {
        int w=(p==1||p==2)?(f.width+1)/2:f.width,h=(p==1||p==2)?(f.height+1)/2:f.height;
        if(f.rgba)w*=4;
        for(int y=0;y<h;y++)av_md5_update(md5,f.planes[p]+y*f.strides[p],w);
    }
    uint8_t digest[16];av_md5_final(md5,digest);
    char hex[33];for(int i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    std::ostringstream result;result<<index<<","<<f.pts<<","<<f.width<<","<<f.height<<","<<(f.rgba?"rgba":(f.planes[3]?"yuva420p":"yuv420p"))<<","<<hex;
    return result.str();
}
// Fork only this single-threaded codec owner, before an actual routed allocation.
// The child resumes the identical successful prefix, injects ENOMEM at this call,
// and must return no output, terminal NoMemory and zero owned bytes. The parent
// verifies the complete successful trace against the same pixel/PTS references.
static bool checkpoint_child = false;
static size_t checkpoint_shard, checkpoint_shards, checkpoint_checked;
static void CheckpointAllocation(size_t index) {
    if ((index-1)%checkpoint_shards!=checkpoint_shard) return;
    const pid_t pid=fork();
    assert(pid>=0);
    if(pid==0) {
        checkpoint_child=true;
        original_source_on_allocation(nullptr);
        original_source_fail_at(index);
        alarm(20);
        return;
    }
    int result=0;
    pid_t waited;
    do { waited=waitpid(pid,&result,0); } while(waited<0 && errno==EINTR);
    if(waited!=pid || !WIFEXITED(result) || WEXITSTATUS(result)!=0) {
        fprintf(stderr,"FAIL checkpoint allocation=%zu child_status=%d\n",index,result);
        abort();
    }
    checkpoint_checked++;
}
static void CheckpointTerminal(OriginalSourceStatus status, OriginalSourceSession& session) {
    if(!checkpoint_child) return;
    assert(status==OriginalSourceStatus::kNoMemory);
    assert(allocation_state.failed() && original_source_injections()==1);
    session.Close();
    assert(original_source_live()==0);
    _exit(0);
}
static size_t CheckpointSweep(const std::string& path, const std::string& reference,
                              const OriginalSourceExpected& expected, size_t allocations,
                              size_t shard, size_t shards) {
    original_source_fault_reset(0);
    checkpoint_shard=shard;checkpoint_shards=shards;checkpoint_checked=0;
    std::ifstream refs(reference);std::string row;int num=0,den=0;
    assert(std::getline(refs,row) && sscanf(row.c_str(),"# timebase=%d/%d",&num,&den)==2);
    OriginalSourceSession session;OriginalSourceFrame frame;unsigned decoded=0;
    original_source_on_allocation(CheckpointAllocation);
    auto status=session.Open(path.c_str(),expected,Lease(),&allocation_state);
    CheckpointTerminal(status,session);
    assert(status==OriginalSourceStatus::kOk);
    while((status=session.Next(&frame))==OriginalSourceStatus::kOk) {
        assert(!checkpoint_child && !allocation_state.failed());
        assert(std::getline(refs,row) && FrameRow(frame,decoded)==row);
        assert(frame.time_base_num==num && frame.time_base_den==den);
        decoded++;
    }
    CheckpointTerminal(status,session);
    assert(status==OriginalSourceStatus::kEnd && decoded==expected.frames);
    assert(!std::getline(refs,row));
    session.Close();
    original_source_on_allocation(nullptr);
    assert(original_source_live()==0 && original_source_injections()==0);
    assert(original_source_calls()==allocations);
    const size_t selected=allocations/shards+(allocations%shards>shard?1:0);
    assert(checkpoint_checked==selected);
    return checkpoint_checked;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const char* shard_env=getenv("ORIGINAL_SOURCE_FAULT_SHARD");
    const char* count_env=getenv("ORIGINAL_SOURCE_FAULT_SHARDS");
    const size_t shard=shard_env?std::stoul(shard_env):0;
    const size_t shards=count_env?std::stoul(count_env):1;
    assert(shards>0 && shard<shards);
    // The production allocator owns hooks and notifies the shared session state.
    original_source_bind(&allocation_state);
    BeginLesson();
    std::ifstream manifest(argv[1]);
    std::string path, hash, reference;
    size_t bytes; unsigned width, height, frames, kind, cases = 0;
    while (manifest >> path >> hash >> bytes >> width >> height >> frames >> kind >> reference) {
        OriginalSourceExpected e{};
        e.bytes = bytes; e.width = width; e.height = height; e.frames = frames;
        e.codec = static_cast<OriginalSourceCodec>(kind);
        for (size_t i=0;i<32;i++) e.sha256[i] = std::stoul(hash.substr(i*2,2),nullptr,16);
        original_source_fault_reset(0);
        OriginalSourceSession s;
        assert(s.Open(path.c_str(),e,Lease(),nullptr)==OriginalSourceStatus::kInvalid);
        if(cases==0) {
            // Without a lesson read lease nothing is read or allocated.
            assert(s.Open(path.c_str(),e,LessonAssetReadLease{},&allocation_state)==OriginalSourceStatus::kLeaseUnavailable);
            assert(original_source_calls()==0 && original_source_live()==0);
            expect_lease_held=true;lease_held_checked=false;
            assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
            expect_lease_held=false;assert(lease_held_checked);
            // The lease ends with Open; decoding continues from the verified snapshot.
            assert(Coordinator().EndLessonSession(kAssignment,kSessionId,lesson_generation));
            OriginalSourceFrame snapshot_frame;
            assert(s.Next(&snapshot_frame)==OriginalSourceStatus::kOk && snapshot_frame.planes[0]);
            assert(!Coordinator().TryRetainLessonSession(kAssignment,kSessionId,lesson_generation));
            OriginalSourceSession stale;
            assert(stale.Open(path.c_str(),e,Coordinator().TryRetainLessonSession(kAssignment,kSessionId,lesson_generation),
                              &allocation_state)==OriginalSourceStatus::kLeaseUnavailable);
            s.Close();assert(original_source_live()==0);
            BeginLesson();
            original_source_fault_reset(0);
            fprintf(stderr,"LEASE refused-empty held-during-snapshot released-after-open stale-refused\n");
        }
        auto status = s.Open(path.c_str(),e,Lease(),&allocation_state);
        if (status != OriginalSourceStatus::kOk) {
            fprintf(stderr,"FAIL verified original Open: status=%d file=%s\n",int(status),path.c_str());
            return 1;
        }
        OriginalSourceFrame f;
        {
            std::ifstream ref(reference); std::string row; unsigned n=0;
            std::getline(ref,row);
            int time_num=0,time_den=0;
            assert(sscanf(row.c_str(),"# timebase=%d/%d",&time_num,&time_den)==2);
            while ((status=s.Next(&f))==OriginalSourceStatus::kOk) {
                assert(f.width==width && f.height==height);
                assert(f.time_base_num==time_num && f.time_base_den==time_den);
                AVMD5* md5=av_md5_alloc(); assert(md5); av_md5_init(md5);
                for(int plane=0;plane<(f.rgba?1:(f.planes[3]?4:3));plane++) {
                    int w=(plane==1||plane==2)?(width+1)/2:width, h=(plane==1||plane==2)?(height+1)/2:height;
                    if(f.rgba)w*=4;
                    for(int y=0;y<h;y++)av_md5_update(md5,f.planes[plane]+y*f.strides[plane],w);
                }
                uint8_t digest[16];av_md5_final(md5,digest);av_free(md5);
                char hex[33];for(int i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
                assert(std::getline(ref,row));
                std::ostringstream actual;actual<<n<<","<<f.pts<<","<<width<<","<<height<<","<<(f.rgba?"rgba":(f.planes[3]?"yuva420p":"yuv420p"))<<","<<hex;
                assert(actual.str()==row);n++;
            }
            assert(status==OriginalSourceStatus::kEnd && n==frames);
            if(e.codec==OriginalSourceCodec::kVp9Alpha) {
                assert(s.Next(&f)==OriginalSourceStatus::kEnd);
                assert(!f.planes[0]);
            }
        }
        s.Close();s.Close();
        assert(original_source_live()==0);
        if(e.codec==OriginalSourceCodec::kVp9Alpha) {
            for(bool eagain : {false,true}) {
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
                delay_output=true;flush_eagain=eagain;
                for(int i=0;i<2;i++){delayed_codec[i]=nullptr;delayed_frame[i]=nullptr;drain_attempted[i]=false;}
                std::ifstream delayed_ref(reference);std::string delayed_row;
                std::getline(delayed_ref,delayed_row);unsigned count=0;
                while((status=s.Next(&f))==OriginalSourceStatus::kOk) {
                    assert(std::getline(delayed_ref,delayed_row) && FrameRow(f,count)==delayed_row);count++;
                }
                delay_output=false;
                assert(status==OriginalSourceStatus::kEnd && count==frames);
                assert(drain_attempted[0] && drain_attempted[1]);
                assert(!delayed_frame[0] && !delayed_frame[1] && original_source_live()==0);
            }
            // Synthetic receive schedules isolate queue refusal from codec buffering.
            for(unsigned count : {4u,5u}) {
                std::atomic<bool> limit_cancel{false};
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&limit_cancel)==OriginalSourceStatus::kOk);
                first_codec=nullptr;synthetic_bytes=1;synthetic_frames=count;synthetic_emitted=0;
                cancel_target=&limit_cancel;
                status=s.Next(&f);
                synthetic_frames=0;cancel_target=nullptr;
                assert(synthetic_emitted==count);
                assert(status==(count==4?OriginalSourceStatus::kCancelled:OriginalSourceStatus::kNoMemory));
                assert(!f.planes[0] && original_source_live()==0);
            }
            for(size_t size : {size_t{4*1024*1024},size_t{4*1024*1024+1}}) {
                std::atomic<bool> limit_cancel{false};
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&limit_cancel)==OriginalSourceStatus::kOk);
                first_codec=nullptr;synthetic_bytes=size;synthetic_frames=1;synthetic_emitted=0;
                cancel_target=&limit_cancel;
                status=s.Next(&f);
                synthetic_frames=0;cancel_target=nullptr;
                assert(status==(size==4*1024*1024?OriginalSourceStatus::kCancelled:OriginalSourceStatus::kNoMemory));
                assert(!f.planes[0] && original_source_live()==0);
            }
            for(int mutation=1;mutation<=6;mutation++) {
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
                mutate_packet=mutation;
                status=s.Next(&f);mutate_packet=0;
                assert(status==OriginalSourceStatus::kMetadata || status==OriginalSourceStatus::kDecode);
                assert(!f.planes[0] && !f.planes[3]);
                s.Close();assert(original_source_live()==0);
            }
            for(int mutation=1;mutation<=4;mutation++) {
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
                mutate_frame=mutation;
                while((status=s.Next(&f))==OriginalSourceStatus::kOk){}
                mutate_frame=0;
                assert(status==OriginalSourceStatus::kMetadata || status==OriginalSourceStatus::kUnsupported);
                assert(!f.planes[0] && !f.planes[3]);
                s.Close();assert(original_source_live()==0);
            }
            assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
            first_codec=nullptr;hold_first_send=true;held_send=false;
            std::ifstream reference_stream(reference);std::string row;
            std::getline(reference_stream,row);unsigned emitted=0;
            while((status=s.Next(&f))==OriginalSourceStatus::kOk) {
                assert(f.alpha && std::getline(reference_stream,row) && FrameRow(f,emitted)==row);
                emitted++;
            }
            hold_first_send=false;
            assert(held_send && emitted==frames && status==OriginalSourceStatus::kEnd);
            assert(original_source_live()==0);
            // Every demux/send/receive checkpoint through both drains is cancellable.
            std::atomic<bool> flag{false};
            cancel_target=&flag;cancel_at=UINT32_MAX;seam_calls=0;
            assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&flag)==OriginalSourceStatus::kOk);
            while(s.Next(&f)==OriginalSourceStatus::kOk){}
            unsigned calls=seam_calls;
            cancel_target=nullptr;cancel_at=0;
            for(unsigned checkpoint : {1u,2u,3u,4u,5u,6u,7u,8u,calls-3,calls-2,calls-1,calls}) {
                flag=false;seam_calls=0;cancel_at=checkpoint;cancel_target=&flag;
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&flag)==OriginalSourceStatus::kOk);
                while((status=s.Next(&f))==OriginalSourceStatus::kOk){}
                assert(flag && status==OriginalSourceStatus::kCancelled && !f.planes[0]);
                cancel_target=nullptr;cancel_at=0;
                assert(original_source_live()==0);
            }
            fprintf(stderr,"VP9 adversarial metadata=6 frame=4 asymmetric-send-EAGAIN=1 checkpoints=12 file=%s\n",path.c_str());
        }
        fprintf(stderr,"SOURCE peak=%zu file=%s\n",original_source_peak(),path.c_str());
        assert(s.Next(nullptr)==OriginalSourceStatus::kInvalid);
        assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
        allocation_state.NotifyFailure();
        OriginalSourceSession sibling;
        assert(sibling.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kNoMemory);
        assert(s.Next(&f)==OriginalSourceStatus::kNoMemory);
        assert(original_source_live()==0);
        assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
        s.Close();
        auto metadata=e;metadata.width--;
        auto metadata_status=s.Open(path.c_str(),metadata,Lease(),&allocation_state);
        if(metadata_status==OriginalSourceStatus::kOk)metadata_status=s.Next(&f);
        assert(metadata_status==OriginalSourceStatus::kMetadata || metadata_status==OriginalSourceStatus::kDecode);
        s.Close();
        char temp[]="/tmp/tbot-original-snapshot.XXXXXX";
        int fd=mkstemp(temp);assert(fd>=0);close(fd);
        {std::ifstream input(path,std::ios::binary);std::ofstream copy(temp,std::ios::binary);copy<<input.rdbuf();}
        assert(s.Open(temp,e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
        {std::ofstream mutation(temp,std::ios::binary|std::ios::trunc);mutation<<"changed";}
        assert(unlink(temp)==0);
        assert(s.Next(&f)==OriginalSourceStatus::kOk);
        s.Close();
        auto bad=e;bad.sha256[0]^=1;assert(s.Open(path.c_str(),bad,Lease(),&allocation_state)==OriginalSourceStatus::kIntegrity);
        bad=e;bad.bytes--;assert(s.Open(path.c_str(),bad,Lease(),&allocation_state)==OriginalSourceStatus::kIntegrity);
        bad=e;bad.bytes=OriginalSourceSession::kMaxBytes+1;assert(s.Open(path.c_str(),bad,Lease(),&allocation_state)==OriginalSourceStatus::kInvalid);
        std::atomic<bool> cancel{true};assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kCancelled);
        cancel=false;assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kOk);
        cancel=true;assert(s.Next(&f)==OriginalSourceStatus::kCancelled);
        cancel=false;cancel_target=&cancel;read_calls=0;cancel_read=2;
        assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kCancelled);
        assert(read_calls==2 && original_source_live()==0);
        cancel_target=nullptr;cancel_read=0;
        original_source_fault_reset(0);
        assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
        const size_t open_allocations=original_source_calls();s.Close();
        for(size_t trigger : {size_t{5},open_allocations}) {
            original_source_fault_reset(0);cancel=false;cancel_target=&cancel;
            cancel_allocation=trigger;original_source_on_allocation(CancelAllocation);
            assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kCancelled);
            original_source_on_allocation(nullptr);cancel_target=nullptr;
            assert(original_source_live()==0);
        }
        {
            original_source_fault_reset(0);
            assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
            const size_t before_next=original_source_calls();
            assert(s.Next(&f)==OriginalSourceStatus::kOk);
            const size_t after_next=original_source_calls();s.Close();
            assert(after_next>before_next);
            for(size_t trigger : {before_next+1,after_next}) {
                original_source_fault_reset(0);cancel=false;
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kOk);
                cancel_target=&cancel;cancel_allocation=trigger;
                original_source_on_allocation(CancelAllocation);
                assert(s.Next(&f)==OriginalSourceStatus::kCancelled);
                original_source_on_allocation(nullptr);cancel_target=nullptr;
                assert(original_source_live()==0);
            }
        }
        {
            cancel=false;cancel_target=&cancel;cancel_send_error=true;
            assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kOk);
            assert(s.Next(&f)==OriginalSourceStatus::kCancelled);
            cancel_target=nullptr;cancel_send_error=false;
            assert(original_source_live()==0);
            for(int terminal : {AVERROR_EOF,AVERROR(EAGAIN)}) {
                cancel=false;cancel_target=&cancel;cancel_receive_code=terminal;
                assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kOk);
                while((status=s.Next(&f))==OriginalSourceStatus::kOk){}
                assert(status==OriginalSourceStatus::kCancelled && cancel);
                cancel_target=nullptr;cancel_receive_code=1;
                assert(original_source_live()==0);
            }
        }
        fprintf(stderr,"CANCEL snapshot-read,demux-open,codec-open,next,receive-eof/eagain file=%s\n",path.c_str());
        for(int i=0;i<3;i++){assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);s.Close();}
        {
            bad=e;bad.frames++;
            assert(s.Open(path.c_str(),bad,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
            while((status=s.Next(&f))==OriginalSourceStatus::kOk){}
            assert(status==OriginalSourceStatus::kMetadata);
        }
        s.Close();assert(original_source_live()==0);
        original_source_fault_reset(0);
        assert(s.Open(path.c_str(),e,Lease(),&allocation_state)==OriginalSourceStatus::kOk);
        while(s.Next(&f)==OriginalSourceStatus::kOk){}
        size_t allocations=original_source_calls();s.Close();
        fprintf(stderr,"FAULT_SWEEP total=%zu file=%s\n",allocations,path.c_str());assert(original_source_live()==0);
        if(e.codec==OriginalSourceCodec::kH264) {
            original_source_fault_reset(0);cancel=false;cancel_target=&cancel;
            cancel_allocation=allocations-3;original_source_on_allocation(CancelAllocation);
            assert(s.Open(path.c_str(),e,Lease(),&allocation_state,&cancel)==OriginalSourceStatus::kOk);
            unsigned emitted=0;
            while((status=s.Next(&f))==OriginalSourceStatus::kOk)emitted++;
            assert(cancel && emitted==e.frames && status==OriginalSourceStatus::kCancelled);
            original_source_on_allocation(nullptr);cancel_target=nullptr;
            assert(original_source_live()==0);
            fprintf(stderr,"CANCEL cleanup allocation=%zu frames=%u\n",allocations-3,emitted);
        }
        size_t checked=0;
        if(getenv("ORIGINAL_SOURCE_FORK_FAULTS")) {
            assert(!getenv("ORIGINAL_SOURCE_CANCEL_ONLY"));
            checked=CheckpointSweep(path,reference,e,allocations,shard,shards);
        } else for(size_t fail=1;fail<=allocations;fail++){
            if(getenv("ORIGINAL_SOURCE_CANCEL_ONLY"))continue;
            if((fail-1)%shards!=shard)continue;
            checked++;
            if(fail%250==0)fprintf(stderr,"FAULT_PROGRESS index=%zu total=%zu\n",fail,allocations);
            alarm(20);
            original_source_fault_reset(fail);
            status=s.Open(path.c_str(),e,Lease(),&allocation_state);
            unsigned decoded=0;
            std::ifstream fault_reference(reference);std::string reference_row;
            std::getline(fault_reference,reference_row);int num=0,den=0;
            assert(sscanf(reference_row.c_str(),"# timebase=%d/%d",&num,&den)==2);
            if(status==OriginalSourceStatus::kOk) {
                while((status=s.Next(&f))==OriginalSourceStatus::kOk){
                    // A decoder may swallow ENOMEM; no frame may escape a sticky fault.
                    assert(!allocation_state.failed());
                    if (!std::getline(fault_reference,reference_row) ||
                        FrameRow(f,decoded)!=reference_row || f.time_base_num!=num || f.time_base_den!=den) {
                        fprintf(stderr,"FAIL fault emitted corrupt prefix: allocation=%zu frame=%u file=%s\n",fail,decoded,path.c_str());
                        return 2;
                    }
                    decoded++;
                }
            }
            assert(original_source_injections()==1);
            assert(status==OriginalSourceStatus::kNoMemory);
            assert(allocation_state.failed());
            s.Close();assert(original_source_live()==0);alarm(0);
        }
        fprintf(stderr,"FAULTS checked=%zu total=%zu shard=%zu/%zu file=%s\n",checked,allocations,shard,shards,path.c_str());
        original_source_fault_reset(0);
        cases++;
    }
    assert(cases==7);
    puts("PASS 7 real sources: integrity, bounded input, complete H264/PNG/VP9-alpha frames, cancellation, repetition");
}
