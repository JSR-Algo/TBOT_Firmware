#include "lesson_mjpeg_mp4.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace tbot {
namespace {

constexpr std::size_t kMaxAtoms = 128;
constexpr std::size_t kMaxTableEntries = kLessonMjpegMp4MaxSamples;
constexpr std::size_t kMaxStscEntries = 32;
constexpr std::uint32_t kMaxTimescale = 1000000;
constexpr std::uint64_t kMaxAtomBytes = kLessonMjpegMp4MaxFileBytes;

constexpr std::uint32_t FourCc(char a, char b, char c, char d) {
    return (static_cast<std::uint32_t>(a) << 24) |
           (static_cast<std::uint32_t>(b) << 16) |
           (static_cast<std::uint32_t>(c) << 8) |
           static_cast<std::uint32_t>(d);
}

std::uint16_t Be16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::uint32_t Be32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t Be64(const std::uint8_t* bytes) {
    return (static_cast<std::uint64_t>(Be32(bytes)) << 32) | Be32(bytes + 4);
}

bool Add(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

struct Atom {
    std::uint32_t type = 0;
    std::uint64_t start = 0;
    std::uint64_t payload = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t end = 0;
};

}  // namespace

class LessonMjpegMp4Parser {
public:
    LessonMjpegMp4Parser(const LessonMjpegMp4Io& io, LessonMjpegMp4Reader* output) : io_(io), output_(output) {}

    LessonMjpegMp4Status Parse() {
        if (io_.read_at == nullptr || io_.file_size < 8 || io_.file_size > kLessonMjpegMp4MaxFileBytes) {
            return io_.file_size > kLessonMjpegMp4MaxFileBytes ? LessonMjpegMp4Status::kLimitExceeded
                                                               : LessonMjpegMp4Status::kInvalidArgument;
        }

        std::uint64_t cursor = 0;
        std::size_t atom_count = 0;
        while (cursor < io_.file_size) {
            Atom atom;
            LessonMjpegMp4Status status = ReadAtom(cursor, io_.file_size, &atom);
            if (status != LessonMjpegMp4Status::kOk) {
                return status;
            }
            if (++atom_count > kMaxAtoms) {
                return LessonMjpegMp4Status::kLimitExceeded;
            }
            switch (atom.type) {
                case FourCc('f', 't', 'y', 'p'):
                    if (have_ftyp_) {
                        return LessonMjpegMp4Status::kMalformed;
                    }
                    have_ftyp_ = true;
                    break;
                case FourCc('m', 'o', 'o', 'v'):
                    if (have_moov_) {
                        return LessonMjpegMp4Status::kMalformed;
                    }
                    have_moov_ = true;
                    status = ParseMoov(atom);
                    if (status != LessonMjpegMp4Status::kOk) {
                        return status;
                    }
                    break;
                case FourCc('m', 'd', 'a', 't'):
                    if (have_mdat_) {
                        return LessonMjpegMp4Status::kUnsupported;
                    }
                    have_mdat_ = true;
                    mdat_start_ = atom.payload;
                    mdat_end_ = atom.end;
                    break;
                case FourCc('m', 'o', 'o', 'f'):
                case FourCc('m', 'v', 'e', 'x'):
                case FourCc('s', 'i', 'd', 'x'):
                    return LessonMjpegMp4Status::kUnsupported;
                case FourCc('f', 'r', 'e', 'e'):
                case FourCc('s', 'k', 'i', 'p'):
                case FourCc('w', 'i', 'd', 'e'):
                    break;
                default:
                    return LessonMjpegMp4Status::kUnsupported;
            }
            cursor = atom.end;
        }

        if (!have_ftyp_ || !have_moov_ || !have_mdat_ || track_count_ != 1 || !have_mvhd_) {
            return LessonMjpegMp4Status::kMalformed;
        }
        return Finalize();
    }

private:
    struct StscEntry {
        std::uint32_t first_chunk;
        std::uint32_t samples_per_chunk;
    };

    bool Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) const {
        return offset <= io_.file_size && size <= io_.file_size - offset && io_.read_at(io_.context, offset, out, size);
    }

    LessonMjpegMp4Status ReadAtom(std::uint64_t cursor, std::uint64_t parent_end, Atom* atom) const {
        if (parent_end < cursor || parent_end - cursor < 8) {
            return LessonMjpegMp4Status::kTruncated;
        }
        std::uint8_t header[16];
        if (!Read(cursor, header, 8)) {
            return LessonMjpegMp4Status::kIoError;
        }
        std::uint64_t size = Be32(header);
        std::uint64_t header_size = 8;
        if (size == 1) {
            if (parent_end - cursor < 16 || !Read(cursor + 8, header + 8, 8)) {
                return LessonMjpegMp4Status::kTruncated;
            }
            size = Be64(header + 8);
            header_size = 16;
        } else if (size == 0) {
            return LessonMjpegMp4Status::kUnsupported;
        }
        if (size < header_size) {
            return LessonMjpegMp4Status::kMalformed;
        }
        if (size > kMaxAtomBytes) {
            return LessonMjpegMp4Status::kLimitExceeded;
        }
        std::uint64_t end = 0;
        if (!Add(cursor, size, &end) || end > parent_end) {
            return LessonMjpegMp4Status::kTruncated;
        }
        atom->type = Be32(header + 4);
        atom->start = cursor;
        atom->payload = cursor + header_size;
        atom->payload_size = size - header_size;
        atom->end = end;
        return LessonMjpegMp4Status::kOk;
    }

    template <typename Callback>
    LessonMjpegMp4Status ScanChildren(const Atom& parent, Callback callback) {
        std::uint64_t cursor = parent.payload;
        std::size_t count = 0;
        while (cursor < parent.end) {
            Atom child;
            LessonMjpegMp4Status status = ReadAtom(cursor, parent.end, &child);
            if (status != LessonMjpegMp4Status::kOk) {
                return status;
            }
            if (++count > kMaxAtoms) {
                return LessonMjpegMp4Status::kLimitExceeded;
            }
            status = callback(child);
            if (status != LessonMjpegMp4Status::kOk) {
                return status;
            }
            cursor = child.end;
        }
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ReadFullBoxPrefix(const Atom& atom, std::uint8_t* version) const {
        std::uint8_t prefix[4];
        if (atom.payload_size < sizeof(prefix) || !Read(atom.payload, prefix, sizeof(prefix))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        *version = prefix[0];
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ParseMoov(const Atom& moov) {
        return ScanChildren(moov, [&](const Atom& atom) {
            switch (atom.type) {
                case FourCc('m', 'v', 'h', 'd'):
                    return ParseMvhd(atom);
                case FourCc('t', 'r', 'a', 'k'):
                    if (++track_count_ > 1) {
                        return LessonMjpegMp4Status::kUnsupported;
                    }
                    return ParseTrak(atom);
                case FourCc('u', 'd', 't', 'a'):
                case FourCc('m', 'e', 't', 'a'):
                    return LessonMjpegMp4Status::kOk;
                case FourCc('m', 'v', 'e', 'x'):
                    return LessonMjpegMp4Status::kUnsupported;
                default:
                    return LessonMjpegMp4Status::kUnsupported;
            }
        });
    }

    LessonMjpegMp4Status ParseMvhd(const Atom& atom) {
        if (have_mvhd_) {
            return LessonMjpegMp4Status::kMalformed;
        }
        std::uint8_t data[20];
        if (atom.payload_size < sizeof(data) || !Read(atom.payload, data, sizeof(data))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        if (data[0] != 0) {
            return LessonMjpegMp4Status::kUnsupported;
        }
        movie_timescale_ = Be32(data + 12);
        movie_duration_ = Be32(data + 16);
        if (movie_timescale_ == 0 || movie_timescale_ > kMaxTimescale || movie_duration_ == 0) {
            return LessonMjpegMp4Status::kMalformed;
        }
        have_mvhd_ = true;
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ParseTrak(const Atom& trak) {
        bool tkhd = false;
        bool mdia = false;
        LessonMjpegMp4Status status = ScanChildren(trak, [&](const Atom& atom) {
            switch (atom.type) {
                case FourCc('t', 'k', 'h', 'd'):
                    if (tkhd) return LessonMjpegMp4Status::kMalformed;
                    tkhd = true;
                    return LessonMjpegMp4Status::kOk;
                case FourCc('m', 'd', 'i', 'a'):
                    if (mdia) return LessonMjpegMp4Status::kMalformed;
                    mdia = true;
                    return ParseMdia(atom);
                case FourCc('e', 'd', 't', 's'):
                    return LessonMjpegMp4Status::kUnsupported;
                default:
                    return LessonMjpegMp4Status::kUnsupported;
            }
        });
        if (status != LessonMjpegMp4Status::kOk) return status;
        return tkhd && mdia ? LessonMjpegMp4Status::kOk : LessonMjpegMp4Status::kMalformed;
    }

    LessonMjpegMp4Status ParseMdia(const Atom& mdia) {
        bool mdhd = false;
        bool hdlr = false;
        bool minf = false;
        LessonMjpegMp4Status status = ScanChildren(mdia, [&](const Atom& atom) {
            switch (atom.type) {
                case FourCc('m', 'd', 'h', 'd'):
                    if (mdhd) return LessonMjpegMp4Status::kMalformed;
                    mdhd = true;
                    return ParseMdhd(atom);
                case FourCc('h', 'd', 'l', 'r'):
                    if (hdlr) return LessonMjpegMp4Status::kMalformed;
                    hdlr = true;
                    return ParseHdlr(atom);
                case FourCc('m', 'i', 'n', 'f'):
                    if (minf) return LessonMjpegMp4Status::kMalformed;
                    minf = true;
                    return ParseMinf(atom);
                default:
                    return LessonMjpegMp4Status::kUnsupported;
            }
        });
        if (status != LessonMjpegMp4Status::kOk) return status;
        return mdhd && hdlr && minf ? LessonMjpegMp4Status::kOk : LessonMjpegMp4Status::kMalformed;
    }

    LessonMjpegMp4Status ParseMdhd(const Atom& atom) {
        std::uint8_t data[20];
        if (atom.payload_size < sizeof(data) || !Read(atom.payload, data, sizeof(data))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        if (data[0] != 0) return LessonMjpegMp4Status::kUnsupported;
        media_timescale_ = Be32(data + 12);
        media_duration_ = Be32(data + 16);
        if (media_timescale_ == 0 || media_timescale_ > kMaxTimescale || media_duration_ == 0) {
            return LessonMjpegMp4Status::kMalformed;
        }
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ParseHdlr(const Atom& atom) {
        std::uint8_t data[12];
        if (atom.payload_size < sizeof(data) || !Read(atom.payload, data, sizeof(data))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        if (data[0] != 0) return LessonMjpegMp4Status::kUnsupported;
        return Be32(data + 8) == FourCc('v', 'i', 'd', 'e') ? LessonMjpegMp4Status::kOk
                                                            : LessonMjpegMp4Status::kUnsupported;
    }

    LessonMjpegMp4Status ParseMinf(const Atom& minf) {
        bool vmhd = false;
        bool stbl = false;
        LessonMjpegMp4Status status = ScanChildren(minf, [&](const Atom& atom) {
            switch (atom.type) {
                case FourCc('v', 'm', 'h', 'd'):
                    vmhd = true;
                    return LessonMjpegMp4Status::kOk;
                case FourCc('d', 'i', 'n', 'f'):
                    return LessonMjpegMp4Status::kOk;
                case FourCc('s', 't', 'b', 'l'):
                    if (stbl) return LessonMjpegMp4Status::kMalformed;
                    stbl = true;
                    return ParseStbl(atom);
                case FourCc('s', 'm', 'h', 'd'):
                    return LessonMjpegMp4Status::kUnsupported;
                default:
                    return LessonMjpegMp4Status::kUnsupported;
            }
        });
        if (status != LessonMjpegMp4Status::kOk) return status;
        return vmhd && stbl ? LessonMjpegMp4Status::kOk : LessonMjpegMp4Status::kMalformed;
    }

    LessonMjpegMp4Status ParseStbl(const Atom& stbl) {
        LessonMjpegMp4Status status = ScanChildren(stbl, [&](const Atom& atom) {
            switch (atom.type) {
                case FourCc('s', 't', 's', 'd'): return ParseStsd(atom);
                case FourCc('s', 't', 't', 's'): return ParseStts(atom);
                case FourCc('s', 't', 's', 'c'): return ParseStsc(atom);
                case FourCc('s', 't', 's', 'z'): return ParseStsz(atom);
                case FourCc('s', 't', 'c', 'o'): return ParseChunkOffsets(atom, false);
                case FourCc('c', 'o', '6', '4'): return ParseChunkOffsets(atom, true);
                case FourCc('s', 't', 's', 's'):
                    return LessonMjpegMp4Status::kUnsupported;
                default:
                    return LessonMjpegMp4Status::kUnsupported;
            }
        });
        return status;
    }

    LessonMjpegMp4Status ParseStsd(const Atom& atom) {
        if (have_stsd_) return LessonMjpegMp4Status::kMalformed;
        std::uint8_t header[16];
        if (atom.payload_size < sizeof(header) || !Read(atom.payload, header, sizeof(header))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        if (header[0] != 0 || Be32(header + 4) != 1) return LessonMjpegMp4Status::kUnsupported;
        const std::uint32_t entry_size = Be32(header + 8);
        const std::uint32_t codec = Be32(header + 12);
        if (entry_size < 78 || entry_size > atom.payload_size - 8) return LessonMjpegMp4Status::kMalformed;
        if (codec != FourCc('j', 'p', 'e', 'g') && codec != FourCc('m', 'j', 'p', 'a')) {
            return LessonMjpegMp4Status::kUnsupported;
        }
        std::uint8_t dimensions[4];
        if (!Read(atom.payload + 8 + 32, dimensions, sizeof(dimensions))) return LessonMjpegMp4Status::kIoError;
        width_ = Be16(dimensions);
        height_ = Be16(dimensions + 2);
        if (width_ == 0 || height_ == 0 || width_ > kLessonMjpegMp4MaxWidth || height_ > kLessonMjpegMp4MaxHeight) {
            return LessonMjpegMp4Status::kLimitExceeded;
        }
        have_stsd_ = true;
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ParseStts(const Atom& atom) {
        if (have_stts_) return LessonMjpegMp4Status::kMalformed;
        std::uint8_t data[16];
        if (atom.payload_size != sizeof(data) || !Read(atom.payload, data, sizeof(data))) {
            return atom.payload_size < sizeof(data) ? LessonMjpegMp4Status::kTruncated
                                                    : LessonMjpegMp4Status::kUnsupported;
        }
        if (data[0] != 0 || Be32(data + 4) != 1) return LessonMjpegMp4Status::kUnsupported;
        stts_sample_count_ = Be32(data + 8);
        sample_delta_ = Be32(data + 12);
        if (stts_sample_count_ == 0 || stts_sample_count_ > kLessonMjpegMp4MaxSamples || sample_delta_ == 0) {
            return stts_sample_count_ > kLessonMjpegMp4MaxSamples ? LessonMjpegMp4Status::kLimitExceeded
                                                                  : LessonMjpegMp4Status::kMalformed;
        }
        have_stts_ = true;
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ParseStsc(const Atom& atom) {
        if (have_stsc_) return LessonMjpegMp4Status::kMalformed;
        std::uint8_t head[8];
        if (atom.payload_size < sizeof(head) || !Read(atom.payload, head, sizeof(head))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        if (head[0] != 0) return LessonMjpegMp4Status::kUnsupported;
        stsc_count_ = Be32(head + 4);
        if (stsc_count_ == 0 || stsc_count_ > kMaxStscEntries || atom.payload_size != 8 + stsc_count_ * 12ULL) {
            return stsc_count_ > kMaxStscEntries ? LessonMjpegMp4Status::kLimitExceeded
                                                 : LessonMjpegMp4Status::kMalformed;
        }
        std::uint32_t previous_first = 0;
        for (std::size_t i = 0; i < stsc_count_; ++i) {
            std::uint8_t entry[12];
            if (!Read(atom.payload + 8 + i * 12, entry, sizeof(entry))) return LessonMjpegMp4Status::kIoError;
            const std::uint32_t first = Be32(entry);
            const std::uint32_t per_chunk = Be32(entry + 4);
            const std::uint32_t description = Be32(entry + 8);
            if (first == 0 || (i == 0 && first != 1) || first <= previous_first || per_chunk == 0 || description != 1) {
                return LessonMjpegMp4Status::kMalformed;
            }
            stsc_[i] = {first, per_chunk};
            previous_first = first;
        }
        have_stsc_ = true;
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ParseStsz(const Atom& atom) {
        if (have_stsz_) return LessonMjpegMp4Status::kMalformed;
        std::uint8_t head[12];
        if (atom.payload_size < sizeof(head) || !Read(atom.payload, head, sizeof(head))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        if (head[0] != 0) return LessonMjpegMp4Status::kUnsupported;
        const std::uint32_t common_size = Be32(head + 4);
        sample_count_ = Be32(head + 8);
        if (sample_count_ == 0 || sample_count_ > kLessonMjpegMp4MaxSamples) {
            return sample_count_ > kLessonMjpegMp4MaxSamples ? LessonMjpegMp4Status::kLimitExceeded
                                                             : LessonMjpegMp4Status::kMalformed;
        }
        const std::uint64_t expected = 12 + (common_size == 0 ? sample_count_ * 4ULL : 0);
        if (atom.payload_size != expected) return LessonMjpegMp4Status::kMalformed;
        for (std::size_t i = 0; i < sample_count_; ++i) {
            std::uint32_t size = common_size;
            if (common_size == 0) {
                std::uint8_t encoded[4];
                if (!Read(atom.payload + 12 + i * 4, encoded, sizeof(encoded))) return LessonMjpegMp4Status::kIoError;
                size = Be32(encoded);
            }
            if (size == 0 || size > kLessonMjpegMp4MaxSampleBytes) {
                return size > kLessonMjpegMp4MaxSampleBytes ? LessonMjpegMp4Status::kLimitExceeded
                                                            : LessonMjpegMp4Status::kMalformed;
            }
            sample_sizes_[i] = size;
        }
        have_stsz_ = true;
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status ParseChunkOffsets(const Atom& atom, bool wide) {
        if (have_offsets_) return LessonMjpegMp4Status::kMalformed;
        std::uint8_t head[8];
        if (atom.payload_size < sizeof(head) || !Read(atom.payload, head, sizeof(head))) {
            return LessonMjpegMp4Status::kTruncated;
        }
        if (head[0] != 0) return LessonMjpegMp4Status::kUnsupported;
        chunk_count_ = Be32(head + 4);
        const std::size_t entry_size = wide ? 8 : 4;
        if (chunk_count_ == 0 || chunk_count_ > kMaxTableEntries ||
            atom.payload_size != 8 + chunk_count_ * entry_size) {
            return chunk_count_ > kMaxTableEntries ? LessonMjpegMp4Status::kLimitExceeded
                                                    : LessonMjpegMp4Status::kMalformed;
        }
        for (std::size_t i = 0; i < chunk_count_; ++i) {
            std::uint8_t encoded[8] = {};
            if (!Read(atom.payload + 8 + i * entry_size, encoded, entry_size)) return LessonMjpegMp4Status::kIoError;
            chunk_offsets_[i] = wide ? Be64(encoded) : Be32(encoded);
        }
        have_offsets_ = true;
        return LessonMjpegMp4Status::kOk;
    }

    LessonMjpegMp4Status Finalize() {
        if (!have_stsd_ || !have_stts_ || !have_stsc_ || !have_stsz_ || !have_offsets_) {
            return LessonMjpegMp4Status::kMalformed;
        }
        if (sample_count_ != stts_sample_count_ || media_timescale_ != movie_timescale_ ||
            media_duration_ != movie_duration_) {
            return LessonMjpegMp4Status::kMetadataMismatch;
        }
        if (sample_count_ > std::numeric_limits<std::uint64_t>::max() / sample_delta_ ||
            static_cast<std::uint64_t>(sample_count_) * sample_delta_ != media_duration_) {
            return LessonMjpegMp4Status::kMetadataMismatch;
        }
        const std::uint64_t fps_scaled = static_cast<std::uint64_t>(media_timescale_) * 1000ULL;
        if (fps_scaled % sample_delta_ != 0 || fps_scaled / sample_delta_ > std::numeric_limits<std::uint32_t>::max()) {
            return LessonMjpegMp4Status::kMetadataMismatch;
        }

        std::size_t sample_index = 0;
        std::uint64_t previous_end = 0;
        for (std::size_t chunk = 0; chunk < chunk_count_; ++chunk) {
            std::size_t stsc_index = 0;
            while (stsc_index + 1 < stsc_count_ && stsc_[stsc_index + 1].first_chunk <= chunk + 1) {
                ++stsc_index;
            }
            std::uint64_t offset = chunk_offsets_[chunk];
            for (std::uint32_t in_chunk = 0; in_chunk < stsc_[stsc_index].samples_per_chunk; ++in_chunk) {
                if (sample_index >= sample_count_) return LessonMjpegMp4Status::kMalformed;
                const std::uint32_t size = sample_sizes_[sample_index];
                std::uint64_t end = 0;
                if (!Add(offset, size, &end) || offset < mdat_start_ || end > mdat_end_ ||
                    (sample_index != 0 && offset < previous_end)) {
                    return LessonMjpegMp4Status::kMetadataMismatch;
                }
                output_->frames_[sample_index] = {offset, size};
                previous_end = end;
                offset = end;
                ++sample_index;
            }
        }
        if (sample_index != sample_count_) return LessonMjpegMp4Status::kMalformed;

        output_->io_ = io_;
        output_->frame_count_ = sample_count_;
        output_->width_ = width_;
        output_->height_ = height_;
        output_->timescale_ = media_timescale_;
        output_->duration_ticks_ = media_duration_;
        output_->frame_duration_ticks_ = sample_delta_;
        output_->fps_milli_ = static_cast<std::uint32_t>(fps_scaled / sample_delta_);
        return LessonMjpegMp4Status::kOk;
    }

    const LessonMjpegMp4Io& io_;
    LessonMjpegMp4Reader* output_;
    bool have_ftyp_ = false;
    bool have_moov_ = false;
    bool have_mdat_ = false;
    bool have_mvhd_ = false;
    bool have_stsd_ = false;
    bool have_stts_ = false;
    bool have_stsc_ = false;
    bool have_stsz_ = false;
    bool have_offsets_ = false;
    std::size_t track_count_ = 0;
    std::uint64_t mdat_start_ = 0;
    std::uint64_t mdat_end_ = 0;
    std::uint32_t movie_timescale_ = 0;
    std::uint64_t movie_duration_ = 0;
    std::uint32_t media_timescale_ = 0;
    std::uint64_t media_duration_ = 0;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    std::uint32_t stts_sample_count_ = 0;
    std::uint32_t sample_delta_ = 0;
    std::size_t sample_count_ = 0;
    std::array<std::uint32_t, kLessonMjpegMp4MaxSamples> sample_sizes_{};
    std::size_t chunk_count_ = 0;
    std::array<std::uint64_t, kLessonMjpegMp4MaxSamples> chunk_offsets_{};
    std::size_t stsc_count_ = 0;
    std::array<StscEntry, kMaxStscEntries> stsc_{};
};

LessonMjpegMp4Status LessonMjpegMp4Reader::Open(const LessonMjpegMp4Io& io) {
    *this = LessonMjpegMp4Reader{};
    if (io.read_at == nullptr || io.file_size == 0) {
        return LessonMjpegMp4Status::kInvalidArgument;
    }
    LessonMjpegMp4Parser parser(io, this);
    return parser.Parse();
}

LessonMjpegMp4Status LessonMjpegMp4Reader::ReadFrame(
    std::size_t index,
    std::uint8_t* destination,
    std::size_t capacity,
    std::size_t* bytes_read
) const {
    if (bytes_read != nullptr) *bytes_read = 0;
    if (index >= frame_count_ || destination == nullptr || bytes_read == nullptr) {
        return LessonMjpegMp4Status::kInvalidArgument;
    }
    const LessonMjpegMp4Frame& selected = frames_[index];
    if (capacity < selected.size) return LessonMjpegMp4Status::kLimitExceeded;
    if (!io_.read_at(io_.context, selected.offset, destination, selected.size)) {
        return LessonMjpegMp4Status::kIoError;
    }
    *bytes_read = selected.size;
    return LessonMjpegMp4Status::kOk;
}

namespace {

void* OpenStdFile(void*, const char* path) {
    return std::fopen(path, "rb");
}

bool SizeStdFile(void*, void* raw_file, std::uint64_t* size) {
    FILE* file = static_cast<FILE*>(raw_file);
    if (fseeko(file, 0, SEEK_END) != 0) {
        return false;
    }
    const off_t end = ftello(file);
    if (end < 0) {
        return false;
    }
    *size = static_cast<std::uint64_t>(end);
    return true;
}

bool SeekStdFile(void*, void* raw_file, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return false;
    }
    return fseeko(static_cast<FILE*>(raw_file), static_cast<off_t>(offset), SEEK_SET) == 0;
}

std::size_t ReadStdFile(void*, void* raw_file, std::uint8_t* out, std::size_t size) {
    return std::fread(out, 1, size, static_cast<FILE*>(raw_file));
}

void CloseStdFile(void*, void* raw_file) {
    std::fclose(static_cast<FILE*>(raw_file));
}

LessonMjpegMp4FileOps DefaultFileOps() {
    return {nullptr, OpenStdFile, SizeStdFile, SeekStdFile, ReadStdFile, CloseStdFile};
}

bool ValidFileOps(const LessonMjpegMp4FileOps& ops) {
    return ops.open != nullptr && ops.size != nullptr && ops.seek != nullptr && ops.read != nullptr &&
           ops.close != nullptr;
}

}  // namespace

LessonMjpegMp4File::~LessonMjpegMp4File() {
    Close();
}

LessonMjpegMp4Status LessonMjpegMp4File::OpenUnderLessonSession(
    const char* path,
    const std::string& assignment_id,
    const std::string& session_id,
    std::uint64_t generation,
    LessonMjpegMp4FileOps ops
) {
    Close();
    if (path == nullptr || path[0] == '\0') {
        return LessonMjpegMp4Status::kInvalidArgument;
    }
    if (ops.open == nullptr) {
        ops = DefaultFileOps();
    }
    if (!ValidFileOps(ops)) {
        return LessonMjpegMp4Status::kInvalidArgument;
    }

    LessonAssetReadLease read_lease = LessonAssetStorageCoordinator::GetInstance().TryRetainLessonSession(
        assignment_id, session_id, generation
    );
    if (!read_lease) {
        return LessonMjpegMp4Status::kLeaseUnavailable;
    }
    void* file = ops.open(ops.context, path);
    if (file == nullptr) {
        return LessonMjpegMp4Status::kIoError;
    }
    std::uint64_t file_size = 0;
    if (!ops.size(ops.context, file, &file_size)) {
        ops.close(ops.context, file);
        return LessonMjpegMp4Status::kIoError;
    }
    if (file_size == 0 || file_size > kLessonMjpegMp4MaxFileBytes) {
        ops.close(ops.context, file);
        return file_size > kLessonMjpegMp4MaxFileBytes ? LessonMjpegMp4Status::kLimitExceeded
                                                       : LessonMjpegMp4Status::kMalformed;
    }

    file_ = file;
    ops_ = ops;
    lesson_read_lease_ = std::move(read_lease);
    const LessonMjpegMp4Status status = reader_.Open({this, ReadAt, file_size});
    if (status != LessonMjpegMp4Status::kOk) {
        Close();
    }
    return status;
}

void LessonMjpegMp4File::Close() {
    reader_ = LessonMjpegMp4Reader{};
    if (file_ != nullptr) {
        ops_.close(ops_.context, file_);
        file_ = nullptr;
    }
    ops_ = {};
    lesson_read_lease_ = {};
}

bool LessonMjpegMp4File::ReadAt(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    auto* self = static_cast<LessonMjpegMp4File*>(context);
    return self != nullptr && self->file_ != nullptr && self->ops_.seek(self->ops_.context, self->file_, offset) &&
           self->ops_.read(self->ops_.context, self->file_, out, size) == size;
}

}  // namespace tbot
