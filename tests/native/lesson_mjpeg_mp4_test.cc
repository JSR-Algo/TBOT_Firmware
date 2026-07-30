#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

#include "lesson_asset_storage_coordinator.h"
#include "lesson_mjpeg_mp4.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson_mjpeg_mp4 test failed: " << message << '\n';
        std::exit(1);
    }
}

void Be32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void Patch32(Bytes& out, std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::uint8_t>(value >> 24);
    out[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    out[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    out[offset + 3] = static_cast<std::uint8_t>(value);
}

Bytes Atom(const char type[5], const Bytes& payload) {
    Bytes out;
    Be32(out, static_cast<std::uint32_t>(payload.size() + 8));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes Join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

Bytes FullBox(std::uint32_t flags, const Bytes& payload) {
    Bytes out;
    Be32(out, flags);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct FixtureOptions {
    const char* sample_entry = "jpeg";
    const char* handler = "vide";
    std::uint32_t track_count = 1;
    std::uint32_t timescale = 1000;
    std::uint32_t duration = 200;
    std::uint32_t sample_delta = 100;
    std::uint32_t sample_count = 2;
    std::uint32_t stts_sample_count = 0;
    std::uint32_t stsc_first_chunk = 1;
    std::uint32_t stsc_samples_per_chunk = 2;
    std::uint32_t chunk_count = 1;
    std::int32_t second_chunk_adjust = 4;
    bool co64 = false;
    bool include_stsd = true;
    bool include_stts = true;
    bool include_stsc = true;
    bool include_stsz = true;
    bool include_offsets = true;
    bool include_edts = false;
    bool include_moof = false;
};

Bytes MakeTrack(const FixtureOptions& options, std::uint32_t chunk_offset) {
    Bytes tkhd(80, 0);
    Patch32(tkhd, 12, 1);

    Bytes mdhd(20, 0);
    Patch32(mdhd, 8, options.timescale);
    Patch32(mdhd, 12, options.duration);

    Bytes hdlr(20, 0);
    std::memcpy(hdlr.data() + 4, options.handler, 4);

    Bytes vmhd = FullBox(1, Bytes(8, 0));
    Bytes url = Atom("url ", FullBox(1, {}));
    Bytes dref_payload;
    Be32(dref_payload, 1);
    dref_payload.insert(dref_payload.end(), url.begin(), url.end());
    Bytes dinf = Atom("dinf", Atom("dref", FullBox(0, dref_payload)));

    Bytes stbl;
    if (options.include_stsd) {
        Bytes entry(78, 0);
        std::memcpy(entry.data() + 4, options.sample_entry, 4);
        entry[14] = 0;
        entry[15] = 1;
        entry[32] = 1;
        entry[33] = 64;
        entry[34] = 0;
        entry[35] = 120;
        Patch32(entry, 0, static_cast<std::uint32_t>(entry.size()));
        Bytes stsd_payload;
        Be32(stsd_payload, 1);
        stsd_payload.insert(stsd_payload.end(), entry.begin(), entry.end());
        Bytes stsd = Atom("stsd", FullBox(0, stsd_payload));
        stbl.insert(stbl.end(), stsd.begin(), stsd.end());
    }
    if (options.include_stts) {
        Bytes payload;
        Be32(payload, 1);
        Be32(payload, options.stts_sample_count == 0 ? options.sample_count : options.stts_sample_count);
        Be32(payload, options.sample_delta);
        Bytes box = Atom("stts", FullBox(0, payload));
        stbl.insert(stbl.end(), box.begin(), box.end());
    }
    if (options.include_stsc) {
        Bytes payload;
        Be32(payload, 1);
        Be32(payload, options.stsc_first_chunk);
        Be32(payload, options.stsc_samples_per_chunk);
        Be32(payload, 1);
        Bytes box = Atom("stsc", FullBox(0, payload));
        stbl.insert(stbl.end(), box.begin(), box.end());
    }
    if (options.include_stsz) {
        Bytes payload;
        Be32(payload, 0);
        Be32(payload, options.sample_count);
        for (std::uint32_t i = 0; i < options.sample_count; ++i) {
            Be32(payload, i == 0 ? 4 : 5);
        }
        Bytes box = Atom("stsz", FullBox(0, payload));
        stbl.insert(stbl.end(), box.begin(), box.end());
    }
    if (options.include_offsets) {
        Bytes payload;
        Be32(payload, options.chunk_count);
        for (std::uint32_t i = 0; i < options.chunk_count; ++i) {
            const std::uint32_t offset = i == 0 ? chunk_offset : chunk_offset + options.second_chunk_adjust;
            if (options.co64) {
                Be32(payload, 0);
                Be32(payload, offset);
            } else {
                Be32(payload, offset);
            }
        }
        Bytes box = Atom(options.co64 ? "co64" : "stco", FullBox(0, payload));
        stbl.insert(stbl.end(), box.begin(), box.end());
    }

    Bytes minf = Atom("minf", Join({Atom("vmhd", vmhd), dinf, Atom("stbl", stbl)}));
    Bytes mdia = Atom("mdia", Join({Atom("mdhd", FullBox(0, mdhd)), Atom("hdlr", FullBox(0, hdlr)), minf}));
    Bytes trak_payload = Join({Atom("tkhd", FullBox(3, tkhd)), mdia});
    if (options.include_edts) {
        Bytes edts = Atom("edts", {});
        trak_payload.insert(trak_payload.begin(), edts.begin(), edts.end());
    }
    return Atom("trak", trak_payload);
}

Bytes MakeMp4(const FixtureOptions& options = {}) {
    const Bytes jpeg1 = {0xff, 0xd8, 0xff, 0xd9};
    const Bytes jpeg2 = {0xff, 0xd8, 0x42, 0xff, 0xd9};
    Bytes ftyp_payload = {'i', 's', 'o', 'm', 0, 0, 0, 0, 'i', 's', 'o', 'm'};
    Bytes ftyp = Atom("ftyp", ftyp_payload);

    Bytes mvhd(96, 0);
    Patch32(mvhd, 8, options.timescale);
    Patch32(mvhd, 12, options.duration);
    Patch32(mvhd, 92, options.track_count + 1);

    Bytes placeholder_track = MakeTrack(options, 0);
    Bytes moov = Atom("moov", Join({Atom("mvhd", FullBox(0, mvhd)), placeholder_track}));
    std::uint32_t data_offset = static_cast<std::uint32_t>(ftyp.size() + moov.size() + 8);
    Bytes tracks;
    for (std::uint32_t i = 0; i < options.track_count; ++i) {
        Bytes track = MakeTrack(options, data_offset);
        tracks.insert(tracks.end(), track.begin(), track.end());
    }
    moov = Atom("moov", Join({Atom("mvhd", FullBox(0, mvhd)), tracks}));
    data_offset = static_cast<std::uint32_t>(ftyp.size() + moov.size() + 8);
    tracks.clear();
    for (std::uint32_t i = 0; i < options.track_count; ++i) {
        Bytes track = MakeTrack(options, data_offset);
        tracks.insert(tracks.end(), track.begin(), track.end());
    }
    moov = Atom("moov", Join({Atom("mvhd", FullBox(0, mvhd)), tracks}));

    Bytes mdat = Atom("mdat", Join({jpeg1, jpeg2}));
    Bytes file = Join({ftyp, moov, mdat});
    if (options.include_moof) {
        Bytes moof = Atom("moof", {});
        file.insert(file.end(), moof.begin(), moof.end());
    }
    return file;
}

bool ReadAt(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    const auto& bytes = *static_cast<const Bytes*>(context);
    if (offset > bytes.size() || size > bytes.size() - static_cast<std::size_t>(offset)) {
        return false;
    }
    std::memcpy(out, bytes.data() + static_cast<std::size_t>(offset), size);
    return true;
}

tbot::LessonMjpegMp4Status Open(const Bytes& bytes, tbot::LessonMjpegMp4Reader* reader) {
    return reader->Open({const_cast<Bytes*>(&bytes), ReadAt, bytes.size()});
}

void ExpectRejected(const Bytes& bytes, const char* message) {
    tbot::LessonMjpegMp4Reader reader;
    Check(Open(bytes, &reader) != tbot::LessonMjpegMp4Status::kOk, message);
}

std::string WriteTempFile(const Bytes& bytes) {
    char path[] = "/tmp/tbot-mjpeg-mp4-XXXXXX";
    const int fd = mkstemp(path);
    Check(fd >= 0, "temporary MP4 opens");
    Check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "temporary MP4 writes");
    Check(close(fd) == 0, "temporary MP4 closes after write");
    return path;
}

struct FakeFile {
    Bytes bytes;
    std::uint64_t cursor = 0;
    bool fail_seek = false;
    bool fail_read = false;
    int closes = 0;
};

void* FakeOpen(void* context, const char*) {
    return context;
}

bool FakeSize(void* context, void*, std::uint64_t* size) {
    *size = static_cast<FakeFile*>(context)->bytes.size();
    return true;
}

bool FakeSeek(void* context, void*, std::uint64_t offset) {
    auto* file = static_cast<FakeFile*>(context);
    if (file->fail_seek || offset > file->bytes.size()) return false;
    file->cursor = offset;
    return true;
}

std::size_t FakeRead(void* context, void*, std::uint8_t* out, std::size_t size) {
    auto* file = static_cast<FakeFile*>(context);
    if (file->fail_read || size > file->bytes.size() - static_cast<std::size_t>(file->cursor)) return 0;
    std::memcpy(out, file->bytes.data() + file->cursor, size);
    file->cursor += size;
    return size;
}

void FakeClose(void* context, void*) {
    ++static_cast<FakeFile*>(context)->closes;
}

tbot::LessonMjpegMp4FileOps FakeOps(FakeFile* file) {
    return {file, FakeOpen, FakeSize, FakeSeek, FakeRead, FakeClose};
}

}  // namespace

int main() {
    Bytes valid = MakeMp4();
    tbot::LessonMjpegMp4Reader reader;
    Check(Open(valid, &reader) == tbot::LessonMjpegMp4Status::kOk, "valid MJPEG MP4 opens");
    Check(reader.frame_count() == 2, "frame count is parsed");
    Check(reader.width() == 320 && reader.height() == 120, "dimensions are parsed");
    Check(reader.timescale() == 1000 && reader.duration_ticks() == 200, "duration is parsed");
    Check(reader.frame_duration_ticks() == 100 && reader.fps_milli() == 10000, "fps is consistent");
    std::uint8_t frame[8] = {};
    std::size_t frame_size = 0;
    Check(reader.ReadFrame(0, frame, sizeof(frame), &frame_size) == tbot::LessonMjpegMp4Status::kOk,
          "first frame reads");
    Check(frame_size == 4 && std::memcmp(frame, "\xff\xd8\xff\xd9", 4) == 0, "first JPEG is exact");
    Check(reader.ReadFrame(1, frame, sizeof(frame), &frame_size) == tbot::LessonMjpegMp4Status::kOk,
          "second frame reads");
    const std::uint8_t jpeg2[] = {0xff, 0xd8, 0x42, 0xff, 0xd9};
    Check(frame_size == sizeof(jpeg2) && std::memcmp(frame, jpeg2, sizeof(jpeg2)) == 0, "second JPEG is exact");

    for (std::size_t size = 0; size < 8; ++size) {
        ExpectRejected(Bytes(valid.begin(), valid.begin() + size), "truncated atom header is rejected");
    }
    Bytes small_atom = valid;
    Patch32(small_atom, 0, 7);
    ExpectRejected(small_atom, "atom smaller than header is rejected");
    Bytes huge_atom = valid;
    Patch32(huge_atom, 0, 1);
    huge_atom.insert(huge_atom.begin() + 8, 8, 0xff);
    ExpectRejected(huge_atom, "oversized 64-bit atom is rejected");

    FixtureOptions options;
    options.sample_entry = "avc1";
    ExpectRejected(MakeMp4(options), "H264 is rejected");
    options = {};
    options.handler = "soun";
    ExpectRejected(MakeMp4(options), "audio track is rejected");
    options = {};
    options.include_moof = true;
    ExpectRejected(MakeMp4(options), "fragments are rejected");
    options = {};
    options.include_edts = true;
    ExpectRejected(MakeMp4(options), "edit lists are rejected");
    options = {};
    options.track_count = 2;
    ExpectRejected(MakeMp4(options), "multiple tracks are rejected");

    options = {};
    options.include_stsd = false;
    ExpectRejected(MakeMp4(options), "missing stsd is rejected");
    options = {};
    options.include_stts = false;
    ExpectRejected(MakeMp4(options), "missing stts is rejected");
    options = {};
    options.include_stsc = false;
    ExpectRejected(MakeMp4(options), "missing stsc is rejected");
    options = {};
    options.include_stsz = false;
    ExpectRejected(MakeMp4(options), "missing stsz is rejected");
    options = {};
    options.include_offsets = false;
    ExpectRejected(MakeMp4(options), "missing chunk offsets are rejected");

    options = {};
    options.timescale = 0;
    ExpectRejected(MakeMp4(options), "zero timescale is rejected");
    options = {};
    options.duration = 201;
    ExpectRejected(MakeMp4(options), "duration mismatch is rejected");
    options = {};
    options.stsc_first_chunk = 2;
    ExpectRejected(MakeMp4(options), "invalid stsc first chunk is rejected");
    options = {};
    options.stsc_samples_per_chunk = 1;
    ExpectRejected(MakeMp4(options), "incomplete stsc coverage is rejected");
    options = {};
    options.stts_sample_count = 1;
    ExpectRejected(MakeMp4(options), "stts and stsz count mismatch is rejected");
    options = {};
    options.sample_count = tbot::kLessonMjpegMp4MaxSamples + 1;
    ExpectRejected(MakeMp4(options), "excessive sample count is rejected");
    options = {};
    options.co64 = true;
    Check(Open(MakeMp4(options), &reader) == tbot::LessonMjpegMp4Status::kOk, "bounded co64 offsets are supported");

    options = {};
    options.chunk_count = 2;
    options.stsc_samples_per_chunk = 1;
    options.second_chunk_adjust = 3;
    ExpectRejected(MakeMp4(options), "overlapping samples are rejected");
    options.second_chunk_adjust = -1;
    ExpectRejected(MakeMp4(options), "non-monotonic samples are rejected");

    Bytes bad_offset = MakeMp4();
    const std::string stco = "stco";
    auto stco_pos = std::search(bad_offset.begin(), bad_offset.end(), stco.begin(), stco.end());
    Check(stco_pos != bad_offset.end(), "stco fixture exists");
    Patch32(bad_offset, static_cast<std::size_t>(stco_pos - bad_offset.begin()) + 12, 0xfffffff0u);
    ExpectRejected(bad_offset, "out-of-file sample offset is rejected");

    Bytes overlapping = MakeMp4();
    const std::string stsz = "stsz";
    auto stsz_pos = std::search(overlapping.begin(), overlapping.end(), stsz.begin(), stsz.end());
    Check(stsz_pos != overlapping.end(), "stsz fixture exists");
    Patch32(overlapping, static_cast<std::size_t>(stsz_pos - overlapping.begin()) + 16, 10);
    ExpectRejected(overlapping, "sample extending beyond media data is rejected");

    Bytes too_many_atoms;
    for (std::size_t i = 0; i < 129; ++i) {
        Bytes free = Atom("free", {});
        too_many_atoms.insert(too_many_atoms.end(), free.begin(), free.end());
    }
    ExpectRejected(too_many_atoms, "atom count bound is enforced");

    FixtureOptions co64_options;
    co64_options.co64 = true;
    Bytes bad_co64 = MakeMp4(co64_options);
    const std::string co64 = "co64";
    auto co64_pos = std::search(bad_co64.begin(), bad_co64.end(), co64.begin(), co64.end());
    Check(co64_pos != bad_co64.end(), "co64 fixture exists");
    Patch32(bad_co64, static_cast<std::size_t>(co64_pos - bad_co64.begin()) + 12, 1);
    ExpectRejected(bad_co64, "co64 offset beyond file is rejected");

    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    coordinator.ForceEndLessonSession();
    const std::string path = WriteTempFile(valid);
    const auto session = coordinator.TryBeginLessonSession("assignment-mp4", "session-mp4");
    Check(session.acquired, "lesson session owns the SD lease");
    {
        tbot::LessonMjpegMp4File file_reader;
        Check(file_reader.OpenUnderLessonSession(path.c_str(), "assignment-mp4", "session-mp4", session.generation) ==
                  tbot::LessonMjpegMp4Status::kOk,
              "production FILE adapter opens under the existing lesson lease");
        Check(!coordinator.EndLessonSession("assignment-mp4", "session-mp4", session.generation),
              "adapter retains the lesson lease until close");
        std::uint8_t file_frame[8] = {};
        std::size_t file_frame_size = 0;
        Check(file_reader.reader().ReadFrame(1, file_frame, sizeof(file_frame), &file_frame_size) ==
                  tbot::LessonMjpegMp4Status::kOk,
              "FILE adapter streams an exact sample");
        Check(file_frame_size == sizeof(jpeg2) && std::memcmp(file_frame, jpeg2, sizeof(jpeg2)) == 0,
              "FILE adapter sample bytes are exact");
        file_reader.Close();
        file_reader.Close();
    }
    Check(coordinator.EndLessonSession("assignment-mp4", "session-mp4", session.generation),
          "adapter close releases its retained lease exactly once");
    auto raw_lease = tbot::SdFatSessionGuard::GetInstance().TryAcquire();
    Check(static_cast<bool>(raw_lease), "SD lease is reusable after session and adapter close");
    raw_lease = {};

    const auto forced_session = coordinator.TryBeginLessonSession("assignment-force", "session-force");
    Check(forced_session.acquired, "forced teardown fixture session acquires");
    tbot::LessonMjpegMp4File forced_reader;
    Check(forced_reader.OpenUnderLessonSession(path.c_str(), "assignment-force", "session-force",
                                               forced_session.generation) == tbot::LessonMjpegMp4Status::kOk,
          "forced teardown fixture adapter opens");
    coordinator.ForceEndLessonSession();
    auto lease_during_forced_close = tbot::SdFatSessionGuard::GetInstance().TryAcquire();
    Check(!lease_during_forced_close, "forced session teardown retains SD ownership while adapter is open");
    forced_reader.Close();
    auto lease_after_forced_close = tbot::SdFatSessionGuard::GetInstance().TryAcquire();
    Check(static_cast<bool>(lease_after_forced_close), "adapter close releases deferred forced-teardown lease");
    lease_after_forced_close = {};

    Bytes truncated(valid.begin(), valid.end() - 2);
    const std::string truncated_path = WriteTempFile(truncated);
    const auto truncated_session = coordinator.TryBeginLessonSession("assignment-short", "session-short");
    Check(truncated_session.acquired, "truncated fixture session acquires");
    tbot::LessonMjpegMp4File truncated_reader;
    Check(truncated_reader.OpenUnderLessonSession(truncated_path.c_str(), "assignment-short", "session-short",
                                                  truncated_session.generation) !=
              tbot::LessonMjpegMp4Status::kOk,
          "truncated file is rejected through FILE adapter");
    Check(coordinator.EndLessonSession("assignment-short", "session-short", truncated_session.generation),
          "failed open releases the retained lease");

    FakeFile seek_failure{valid};
    seek_failure.fail_seek = true;
    const auto seek_session = coordinator.TryBeginLessonSession("assignment-seek", "session-seek");
    Check(seek_session.acquired, "seek failure fixture session acquires");
    tbot::LessonMjpegMp4File seek_reader;
    Check(seek_reader.OpenUnderLessonSession("fake", "assignment-seek", "session-seek", seek_session.generation,
                                             FakeOps(&seek_failure)) == tbot::LessonMjpegMp4Status::kIoError,
          "seek failure is reported");
    Check(seek_failure.closes == 1, "seek failure closes the file once");
    Check(coordinator.EndLessonSession("assignment-seek", "session-seek", seek_session.generation),
          "seek failure releases the retained lease");

    FakeFile read_failure{valid};
    read_failure.fail_read = true;
    const auto read_session = coordinator.TryBeginLessonSession("assignment-read", "session-read");
    Check(read_session.acquired, "read failure fixture session acquires");
    tbot::LessonMjpegMp4File read_reader;
    Check(read_reader.OpenUnderLessonSession("fake", "assignment-read", "session-read", read_session.generation,
                                             FakeOps(&read_failure)) == tbot::LessonMjpegMp4Status::kIoError,
          "read failure is reported");
    Check(read_failure.closes == 1, "read failure closes the file once");
    Check(coordinator.EndLessonSession("assignment-read", "session-read", read_session.generation),
          "read failure releases the retained lease");

    unlink(path.c_str());
    unlink(truncated_path.c_str());

    std::cout << "lesson_mjpeg_mp4 test passed\n";
    return 0;
}
