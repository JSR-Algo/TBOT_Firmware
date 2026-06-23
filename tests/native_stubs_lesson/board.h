#pragma once
// Host stub for main/boards/common/board.h. Provides Board::GetInstance() with a
// settable Display* and a scriptable Network/Http so FetchLessonImage's HTTP path is
// reachable. The REAL on-wire fetch (TLS/chunked transfer) is a live-network boundary
// excluded by the no-real-network rule; this fake reproduces the status/length/read
// state machine the renderer drives.
#include "display.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ----- scriptable fake HTTP response -----
struct HostHttpOpenCall {
    std::string method;
    std::string url;
};

struct HostHttpScript {
    bool create_null = false;     // CreateHttp returns null
    bool open_ok = true;          // Open() result
    int status = 200;             // GetStatusCode()
    bool use_content_length = true;  // known length vs chunked (length==0) path
    std::vector<unsigned char> body;  // bytes the server "sends"
    long content_length_override = -1;  // if >=0, report this as GetBodyLength (else body.size or 0)
    int read_error_after = -1;    // if >=0, the Nth Read() returns -1 (read error)
    int early_close_after = -1;   // if >=0, the Nth Read() returns 0 (server closed early)
    size_t max_chunk = 0;         // 0 = serve all remaining in one Read; else cap each Read
    std::vector<HostHttpOpenCall> open_calls;
};

inline HostHttpScript& HostHttp() {
    static HostHttpScript s;
    return s;
}
inline void ResetHostHttp() { HostHttp() = HostHttpScript{}; }

class FakeHttp {
public:
    bool Open(const char* method, const char* url) {
        HostHttp().open_calls.push_back({method ? method : "", url ? url : ""});
        opened_ = HostHttp().open_ok;
        return opened_;
    }
    int GetStatusCode() { return HostHttp().status; }
    size_t GetBodyLength() {
        const auto& s = HostHttp();
        if (!s.use_content_length) return 0;  // chunked path
        if (s.content_length_override >= 0) return static_cast<size_t>(s.content_length_override);
        return s.body.size();
    }
    int Read(char* buf, size_t want) {
        auto& s = HostHttp();
        if (s.read_error_after >= 0 && read_calls_ >= s.read_error_after) {
            read_calls_++;
            return -1;
        }
        if (s.early_close_after >= 0 && read_calls_ >= s.early_close_after) {
            read_calls_++;
            return 0;  // server closed early / EOF
        }
        read_calls_++;
        size_t remaining = (pos_ < s.body.size()) ? (s.body.size() - pos_) : 0;
        if (remaining == 0) return 0;  // EOF
        size_t n = remaining < want ? remaining : want;
        if (s.max_chunk > 0 && n > s.max_chunk) n = s.max_chunk;
        std::memcpy(buf, s.body.data() + pos_, n);
        pos_ += n;
        return static_cast<int>(n);
    }
    void Close() { closed_ = true; }

    bool opened_ = false;
    bool closed_ = false;

private:
    size_t pos_ = 0;
    int read_calls_ = 0;
};

class NetworkInterface {
public:
    std::unique_ptr<FakeHttp> CreateHttp(int /*priority*/) {
        if (HostHttp().create_null) return nullptr;
        return std::make_unique<FakeHttp>();
    }
};

class Board {
public:
    static Board& GetInstance() {
        static Board inst;
        return inst;
    }

    Display* display_ = nullptr;          // set by test (LvglDisplay*, Display*, or null)
    NetworkInterface* network_ = nullptr; // set by test (null => "no network" branch)

    Display* GetDisplay() { return display_; }
    NetworkInterface* GetNetwork() { return network_; }
};
