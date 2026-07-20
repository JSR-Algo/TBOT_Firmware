#include "lesson_asset_download_raii.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct FakeHttp {
    int closes = 0;
    void Close() { ++closes; }
};

int g_file_closes = 0;
int g_heap_frees = 0;
int g_removes = 0;
int g_renames = 0;
bool g_rename_fails = false;

int CountFileClose(FILE*) {
    ++g_file_closes;
    return 0;
}

void CountHeapFree(void*) {
    ++g_heap_frees;
}

int CountRemove(const char*) {
    ++g_removes;
    return 0;
}

int CountRename(const char*, const char*) {
    ++g_renames;
    return g_rename_fails ? -1 : 0;
}

void TestExceptionUnwindClosesEveryResourceAndRemovesTemp() {
    FakeHttp http;
    g_file_closes = 0;
    g_heap_frees = 0;
    g_removes = 0;
    try {
        ScopedHttpClose<FakeHttp> http_close(&http);
        ScopedTempPath temp("asset.tmp", CountRemove, CountRename);
        ScopedCFile file(reinterpret_cast<FILE*>(0x1), CountFileClose);
        ScopedHeapAllocation buffer(reinterpret_cast<void*>(0x2), CountHeapFree);
        throw std::bad_alloc();
    } catch (const std::bad_alloc&) {
    }
    Require(http.closes == 1, "HTTP closed exactly once on bad_alloc");
    Require(g_file_closes == 1, "FILE closed exactly once on bad_alloc");
    Require(g_heap_frees == 1, "heap buffer freed exactly once on bad_alloc");
    Require(g_removes == 1, "temp removed exactly once on bad_alloc");
}

void TestSuccessfulCommitDisarmsTempCleanup() {
    g_removes = 0;
    g_renames = 0;
    g_rename_fails = false;
    {
        ScopedTempPath temp("asset.tmp", CountRemove, CountRename);
        temp.CommitTo("asset.download");
    }
    Require(g_renames == 1, "successful commit renames once");
    Require(g_removes == 1, "successful commit removes destination only");
}

void TestFailedCommitStillCleansTemp() {
    g_removes = 0;
    g_renames = 0;
    g_rename_fails = true;
    bool threw = false;
    try {
        ScopedTempPath temp("asset.tmp", CountRemove, CountRename);
        temp.CommitTo("asset.download");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw, "failed rename throws");
    Require(g_renames == 1, "failed commit renames once");
    Require(g_removes == 2, "failed commit removes destination and temp");
}

}  // namespace

int main() {
    TestExceptionUnwindClosesEveryResourceAndRemovesTemp();
    TestSuccessfulCommitDisarmsTempCleanup();
    TestFailedCommitStillCleansTemp();
    std::cout << "lesson asset download RAII host tests passed\n";
    return 0;
}
