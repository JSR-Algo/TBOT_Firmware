#include "lesson_asset_sample_url_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    Require(IsAllowedSampleLessonAssetBaseUrl("https://assets.example.com/sample"),
            "https base accepted");
    Require(IsAllowedSampleLessonAssetBaseUrl("http://localhost:8080/sample/"),
            "http localhost base accepted");

    const char* rejected[] = {
        "",
        "file:///tmp/assets",
        "ftp://assets.example.com/sample",
        "assets.example.com/sample",
        "https://user:pass@assets.example.com/sample",
        "https://assets.example.com/sample\n",
        "https://assets.example.com/sample pack",
        "https://assets.example.com\\sample",
        "https:///sample",
        "https://[::1/sample",
        "https://assets.example.com:bad/sample",
        "https://assets.example.com:70000/sample",
        "https://assets.example.com/sample?digest=caller",
        "https://assets.example.com/sample#fragment",
    };
    for (const char* value : rejected) {
        Require(!IsAllowedSampleLessonAssetBaseUrl(value), value);
    }

    std::cout << "lesson asset sample URL policy host tests passed\n";
    return 0;
}
