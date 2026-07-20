#include "lesson_asset_sync_path_policy.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kChecksum =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string Key(const std::string& slug = "pip-farm-3m") {
    return slug + "/v1-" + kChecksum;
}

std::string Path(const std::string& basename) {
    return "/sdcard/tbot/lesson-assets/" + Key() + "/" + basename;
}

void TestAcceptsOnlyExactDirectFileDestination() {
    const std::vector<std::pair<std::string, std::string>> valid = {
        {Path("backgroundScene.poster"), "backgroundScene.poster"},
        {Path("teachingObject.barn"), "teachingObject.barn"},
        {Path("robotOverlay.teach"), "robotOverlay.teach"},
        {Path("space%20safe.png"), "space safe.png"},
    };
    for (const auto& [path, key] : valid) {
        const auto result = ValidateLessonAssetSyncPath(Key(), path, key);
        Expect(result.code == LessonAssetSyncPathCode::kValid, "valid path rejected");
        Expect(result.destination == path, "valid path was rewritten");
    }
}

void TestRejectsForeignRootsKeysAndDirectoryTargets() {
    const std::vector<std::string> invalid = {
        "/sdcard/tbot/lesson-assets/current.json",
        "/sdcard/tbot/lesson-assets/pip-farm-3m/pvg.json",
        "/sdcard/tbot/lesson-assets/" + Key("other") + "/poster.jpg",
        "/sdcard/tbot/lesson-assets/" + Key() + "/nested/poster.jpg",
        "/sdcard/tbot/lesson-assets/" + Key() + "/../poster.jpg",
        "/sdcard/tbot/lesson-assets/" + Key() + "/",
        "sd://sdcard/tbot/lesson-assets/" + Key() + "/poster.jpg",
        "file:///sdcard/tbot/lesson-assets/" + Key() + "/poster.jpg",
        "/sdcard/tbot/lesson-assets/" + Key() + "//poster.jpg",
        "/sdcard/tbot/lesson-assets/" + Key() + "/poster\\.jpg",
    };
    for (const auto& path : invalid) {
        Expect(ValidateLessonAssetSyncPath(Key(), path, "poster.jpg").code !=
                   LessonAssetSyncPathCode::kValid,
               "unsafe path accepted");
    }
}

void TestRejectsInvalidKeysWithoutNormalizing() {
    const std::vector<std::string> invalid = {
        " " + Key(),
        Key() + " ",
        "Pip-farm-3m/v1-" + std::string(kChecksum),
        "pip-farm-3m/v01-" + std::string(kChecksum),
        "pip-farm-3m/v1-" + std::string(64, 'A'),
        "sd://" + Key(),
    };
    for (const auto& key : invalid) {
        Expect(ValidateLessonAssetSyncPath(key, Path("poster.jpg"), "poster.jpg").code ==
                   LessonAssetSyncPathCode::kInvalidCacheKey,
               "invalid cache key accepted");
    }
}

void TestRejectsReservedCallerControlledNames() {
    const std::vector<std::string> invalid = {
        "current.json",
        "pvg.json",
        "activation.json",
        "lesson-pack-activation.json",
        "poster.jpg.tmp",
        "poster.jpg.download",
        "poster.jpg.part",
        "poster.backup",
        "poster.BACKUP",
        ".tmp",
        ".download",
        ".",
        "..",
    };
    for (const auto& basename : invalid) {
        Expect(ValidateLessonAssetSyncPath(Key(), Path(basename), basename).code ==
                   LessonAssetSyncPathCode::kReservedDestination,
               "reserved destination accepted");
    }
}

void TestRejectsBackupPairInEitherPackOrder() {
    for (const auto& pair : std::vector<std::vector<std::string>>{
             {"poster", "poster.backup"},
             {"poster.backup", "poster"},
         }) {
        bool rejected = false;
        std::vector<std::string> accepted;
        for (const auto& basename : pair) {
            const auto result = ValidateLessonAssetSyncPath(
                Key(), Path(basename), basename);
            if (result.code != LessonAssetSyncPathCode::kValid) {
                rejected = true;
                break;
            }
            for (const auto& destination : accepted) {
                if (LessonAssetSyncDestinationsCollide(
                        destination, result.destination)) {
                    rejected = true;
                    break;
                }
            }
            if (rejected) {
                break;
            }
            accepted.push_back(result.destination);
        }
        Expect(rejected, "poster/backup pair accepted in one pack order");
    }
}

void TestRejectsEveryFatTrailingDotAlias() {
    const std::vector<std::string> invalid = {
        "poster.jpg.",
        "poster.jpg..",
        "Poster.JPG.",
        "CURRENT.JSON.",
        "poster.jpg.DOWNLOAD.",
    };
    for (const auto& basename : invalid) {
        Expect(ValidateLessonAssetSyncPath(Key(), Path(basename), basename).code ==
                   LessonAssetSyncPathCode::kReservedDestination,
               "FAT trailing-dot alias accepted");
    }
}

void TestRejectsEmbeddedNulAndFatCaseCollisions() {
    std::string path = Path("poster.jpg");
    path.push_back('\0');
    path.append(".tmp");
    Expect(ValidateLessonAssetSyncPath(Key(), path, "poster.jpg").code !=
               LessonAssetSyncPathCode::kValid,
           "embedded NUL accepted");
    Expect(LessonAssetSyncDestinationsCollide(Path("Poster.JPG"), Path("poster.jpg")),
           "FAT case collision missed");
    Expect(LessonAssetSyncDestinationsCollide(Path("poster.jpg"), Path("POSTER.JPG.")),
           "two-asset FAT trailing-dot collision missed");
    Expect(LessonAssetSyncDestinationsCollide(Path("poster.jpg."), Path("poster.jpg...")),
           "two-asset repeated trailing-dot collision missed");
    Expect(!LessonAssetSyncDestinationsCollide(Path("poster.jpg"), Path("overlay.png")),
           "distinct destinations collide");
}

void TestPercentEncodingIsLiteralValidatedAndNeverDecoded() {
    const auto encoded_separator = Path("folder%2Fposter%20one.png");
    const auto separator = ValidateLessonAssetSyncPath(
        Key(), encoded_separator, "folder/poster one.png");
    Expect(separator.code == LessonAssetSyncPathCode::kValid,
           "encoded separator key rejected");
    Expect(separator.destination == encoded_separator,
           "encoded separator destination was decoded");

    const auto encoded_backslash = Path("folder%5Cposter.png");
    Expect(ValidateLessonAssetSyncPath(
               Key(), encoded_backslash, "folder\\poster.png").code ==
               LessonAssetSyncPathCode::kValid,
           "encoded backslash key rejected");
    const auto encoded_dot = Path("%2E%2E.png");
    Expect(ValidateLessonAssetSyncPath(Key(), encoded_dot, "...png").code ==
               LessonAssetSyncPathCode::kValid,
           "encoded dot bytes rejected");
}

void TestRejectsMalformedPercentRawUnsafeBytesAndKeyMismatch() {
    const std::vector<std::string> invalid = {
        "poster%.jpg",
        "poster%2.jpg",
        "poster%GG.jpg",
        "poster name.jpg",
        "poster\tname.jpg",
        "poster:name.jpg",
        "caf\xC3\xA9.png",
    };
    for (const auto& basename : invalid) {
        Expect(ValidateLessonAssetSyncPath(Key(), Path(basename), basename).code !=
                   LessonAssetSyncPathCode::kValid,
               "unsafe raw basename accepted");
    }
    Expect(ValidateLessonAssetSyncPath(
               Key(), Path("poster.jpg"), "other.jpg").code !=
               LessonAssetSyncPathCode::kValid,
           "destination not bound to asset key");
    Expect(ValidateLessonAssetSyncPath(
               Key(), Path("folder%2fposter.png"), "folder/poster.png").code !=
               LessonAssetSyncPathCode::kValid,
           "non-canonical lowercase percent hex accepted");
}

void TestExactHashAndUrlPolicies() {
    Expect(IsExactLowerLessonAssetSha256(std::string(kChecksum)), "valid sha rejected");
    Expect(!IsExactLowerLessonAssetSha256(std::string(64, 'A')), "uppercase sha accepted");
    Expect(!IsExactLowerLessonAssetSha256(" " + std::string(kChecksum)),
           "trimmed sha accepted");

    Expect(IsAllowedLessonAssetSyncUrl("https://assets.example/a%20b.png?token=x"),
           "valid https URL rejected");
    Expect(IsAllowedLessonAssetSyncUrl("http://127.0.0.1:8000/a.png"),
           "valid http URL rejected");
    for (const auto& url : std::vector<std::string>{
             "ftp://assets.example/a.png",
             "https://user@assets.example/a.png",
             "https://assets.example/a b.png",
             "https://assets.example/a\\b.png",
             "https:///missing-host.png",
             "https://:443/missing-host.png",
             " HTTPS://assets.example/a.png",
         }) {
        Expect(!IsAllowedLessonAssetSyncUrl(url), "unsafe URL accepted");
    }
}

}  // namespace

int main() {
    TestAcceptsOnlyExactDirectFileDestination();
    TestRejectsForeignRootsKeysAndDirectoryTargets();
    TestRejectsInvalidKeysWithoutNormalizing();
    TestRejectsReservedCallerControlledNames();
    TestRejectsBackupPairInEitherPackOrder();
    TestRejectsEveryFatTrailingDotAlias();
    TestRejectsEmbeddedNulAndFatCaseCollisions();
    TestPercentEncodingIsLiteralValidatedAndNeverDecoded();
    TestRejectsMalformedPercentRawUnsafeBytesAndKeyMismatch();
    TestExactHashAndUrlPolicies();
    std::cout << "lesson asset sync path host tests passed\n";
    return 0;
}
