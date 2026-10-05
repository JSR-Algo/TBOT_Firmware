#include "lesson_original_source_contract.h"

#include "lesson_asset_cache_evict.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>

namespace tbot {
namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr std::size_t kMaxReasonBytes = 64;
constexpr std::size_t kMaxCueBytes = 128;
const char* const kClipRoles[] = {"flight", "walking", "greeting-teaching", "celebration"};

bool ExactKeys(const cJSON* object, std::initializer_list<const char*> keys) {
    if (!cJSON_IsObject(object)) return false;
    std::size_t count = 0;
    for (const cJSON* item = object->child; item != nullptr; item = item->next) ++count;
    if (count != keys.size()) return false;
    for (const char* key : keys) {
        if (cJSON_GetObjectItemCaseSensitive(object, key) == nullptr) return false;
    }
    // Duplicate names would satisfy the count with a missing key above.
    std::set<std::string> seen;
    for (const cJSON* item = object->child; item != nullptr; item = item->next) {
        if (item->string == nullptr || !seen.insert(item->string).second) return false;
    }
    return true;
}

const cJSON* Get(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

bool Integer(const cJSON* value, double minimum, double maximum, std::uint64_t* out = nullptr) {
    if (!cJSON_IsNumber(value)) return false;
    const double number = value->valuedouble;
    if (!std::isfinite(number) || std::trunc(number) != number || number < minimum || number > maximum)
        return false;
    if (out != nullptr) *out = static_cast<std::uint64_t>(number);
    return true;
}

bool IntegerEquals(const cJSON* value, double expected) {
    return Integer(value, expected, expected);
}

bool Normalized(const cJSON* value) {
    return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) && value->valuedouble >= 0 &&
           value->valuedouble <= 1;
}

bool String(const cJSON* value, std::string* out = nullptr) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0') return false;
    if (out != nullptr) *out = value->valuestring;
    return true;
}

bool StringEquals(const cJSON* value, const char* expected) {
    return cJSON_IsString(value) && value->valuestring != nullptr && std::strcmp(value->valuestring, expected) == 0;
}

bool Strings(const cJSON* value) {
    if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) == 0) return false;
    for (const cJSON* item = value->child; item != nullptr; item = item->next) {
        if (!String(item)) return false;
    }
    return true;
}

bool LowerHex64(const cJSON* value) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr || std::strlen(value->valuestring) != 64)
        return false;
    for (const char* c = value->valuestring; *c != '\0'; ++c) {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return false;
    }
    return true;
}

bool UuidV4(const cJSON* value) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
    const char* s = value->valuestring;
    if (std::strlen(s) != 36) return false;
    for (int i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return s[14] == '4' && (s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');
}

bool Slug(const char* value, std::size_t max_bytes) {
    if (value == nullptr || value[0] == '\0' || std::strlen(value) > max_bytes) return false;
    bool previous_dash = true;
    for (const char* c = value; *c != '\0'; ++c) {
        const bool alnum = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9');
        if (alnum) previous_dash = false;
        else if (*c == '-' && !previous_dash && c[1] != '\0') previous_dash = true;
        else return false;
    }
    return true;
}

struct Pose { double x, y, scale; };

bool Point(const cJSON* value, bool pose, Pose* out) {
    if (!(pose ? ExactKeys(value, {"x", "y", "scale"}) : ExactKeys(value, {"x", "y"}))) return false;
    const cJSON* x = Get(value, "x");
    const cJSON* y = Get(value, "y");
    if (!Normalized(x) || !Normalized(y)) return false;
    out->x = x->valuedouble;
    out->y = y->valuedouble;
    out->scale = 0;
    if (pose) {
        const cJSON* scale = Get(value, "scale");
        if (!cJSON_IsNumber(scale) || !std::isfinite(scale->valuedouble) || scale->valuedouble <= 0 ||
            scale->valuedouble > 2) return false;
        out->scale = scale->valuedouble;
    }
    return true;
}

bool PoseFrom(const cJSON* value, std::initializer_list<const char*> keys, Pose* out) {
    if (!ExactKeys(value, keys)) return false;
    cJSON* pose = cJSON_CreateObject();
    if (pose == nullptr) return false;
    bool ok = true;
    for (const char* key : {"x", "y", "scale"}) {
        cJSON* copy = cJSON_Duplicate(Get(value, key), false);
        if (copy == nullptr) { ok = false; break; }
        cJSON_AddItemToObject(pose, key, copy);
    }
    ok = ok && Point(pose, true, out);
    cJSON_Delete(pose);
    return ok;
}

bool Same(const Pose& a, const Pose& b) { return a.x == b.x && a.y == b.y && a.scale == b.scale; }

struct JourneyAsset { std::string sha256, media_type; bool has_size; double width, height; };

const char* SourceAsset(const cJSON* value, const char* media_type, JourneyAsset* out, std::string* id) {
    if (!ExactKeys(value, {"assetVersionId", "sha256", "mediaType", "width", "height"}))
        return "source asset keys";
    if (!UuidV4(Get(value, "assetVersionId")) || !LowerHex64(Get(value, "sha256")))
        return "source asset identity";
    if (!StringEquals(Get(value, "mediaType"), media_type)) return "source asset media type";
    if (!Integer(Get(value, "width"), 1, kMaxSafeInteger) || !Integer(Get(value, "height"), 1, kMaxSafeInteger))
        return "source asset dimensions";
    *id = Get(value, "assetVersionId")->valuestring;
    *out = {Get(value, "sha256")->valuestring, media_type, true, Get(value, "width")->valuedouble,
            Get(value, "height")->valuedouble};
    return nullptr;
}

const char* ScenePath(const cJSON* scene) {
    if (!ExactKeys(scene, {"flightIngress", "landing", "walk", "teachingAnchor", "objectAnchor", "safeZone"}))
        return "scenePath keys";
    const cJSON* flight = Get(scene, "flightIngress");
    if (!ExactKeys(flight, {"durationMs", "start", "mid", "end"}) || !IntegerEquals(Get(flight, "durationMs"), 3200))
        return "flight";
    Pose start{}, mid{}, end{}, landing{};
    if (!Point(Get(flight, "start"), true, &start) || !Point(Get(flight, "mid"), true, &mid) ||
        !Point(Get(flight, "end"), true, &end)) return "flight pose";
    const cJSON* land = Get(scene, "landing");
    if (!cJSON_IsObject(land) || !IntegerEquals(Get(land, "durationMs"), 500)) return "landing";
    {
        cJSON* pose = cJSON_Duplicate(land, true);
        if (pose == nullptr) return "allocation";
        cJSON_DeleteItemFromObjectCaseSensitive(pose, "durationMs");
        const bool ok = ExactKeys(land, {"x", "y", "scale", "durationMs"}) && Point(pose, true, &landing);
        cJSON_Delete(pose);
        if (!ok) return "landing pose";
    }
    if (!Same(end, landing)) return "flight and landing endpoints differ";
    const cJSON* walk = Get(scene, "walk");
    if (!ExactKeys(walk, {"durationMs", "keyframes"}) || !IntegerEquals(Get(walk, "durationMs"), 5000))
        return "walk";
    const cJSON* frames = Get(walk, "keyframes");
    if (!cJSON_IsArray(frames) || cJSON_GetArraySize(frames) < 2) return "walk keyframes";
    double previous = -1;
    Pose first{};
    bool have_first = false;
    double last_time = -1;
    for (const cJSON* frame = frames->child; frame != nullptr; frame = frame->next) {
        std::uint64_t time = 0;
        if (!cJSON_IsObject(frame) || !Integer(Get(frame, "timeMs"), 0, kMaxSafeInteger, &time) ||
            static_cast<double>(time) <= previous) return "walk keyframe time";
        previous = static_cast<double>(time);
        last_time = previous;
        Pose pose{};
        if (!PoseFrom(frame, {"timeMs", "x", "y", "scale"}, &pose)) return "walk keyframe pose";
        if (!have_first) { first = pose; have_first = true; }
    }
    if (!IntegerEquals(Get(cJSON_GetArrayItem(frames, 0), "timeMs"), 0) || last_time != 5000)
        return "walk endpoints";
    if (!Same(first, landing)) return "walk must begin at landing";
    Pose teaching{}, object{};
    if (!Point(Get(scene, "teachingAnchor"), false, &teaching) || !Point(Get(scene, "objectAnchor"), false, &object))
        return "anchors";
    const cJSON* safe = Get(scene, "safeZone");
    if (!ExactKeys(safe, {"x", "y", "width", "height"})) return "safeZone keys";
    for (const char* key : {"x", "y", "width", "height"}) {
        if (!Normalized(Get(safe, key))) return "safeZone values";
    }
    const double sx = Get(safe, "x")->valuedouble, sy = Get(safe, "y")->valuedouble;
    const double sw = Get(safe, "width")->valuedouble, sh = Get(safe, "height")->valuedouble;
    if (sw == 0 || sh == 0 || sx + sw > 1 || sy + sh > 1) return "safeZone outside stage";
    for (const Pose& anchor : {teaching, object}) {
        if (anchor.x < sx || anchor.x > sx + sw || anchor.y < sy || anchor.y > sy + sh)
            return "anchor outside safeZone";
    }
    return nullptr;
}

const char* Journey(const cJSON* journey, std::map<std::string, JourneyAsset>* assets, std::size_t* steps_out) {
    if (!ExactKeys(journey, {"presetId", "presetVersion", "assets", "scenePath", "boundedContext", "steps"}))
        return "journey keys";
    if (!StringEquals(Get(journey, "presetId"), "tvideoJourney") || !IntegerEquals(Get(journey, "presetVersion"), 1))
        return "journey preset";
    const cJSON* journey_assets = Get(journey, "assets");
    if (!ExactKeys(journey_assets, {"background", "robotClips"})) return "journey assets";
    JourneyAsset background{};
    std::string id;
    if (const char* error = SourceAsset(Get(journey_assets, "background"), "video/mp4", &background, &id)) return error;
    if (background.width != 480 || background.height != 320) return "background must be 480x320";
    assets->emplace(id, background);
    const cJSON* clips = Get(journey_assets, "robotClips");
    if (!cJSON_IsArray(clips) || cJSON_GetArraySize(clips) != 4) return "robot clip count";
    std::set<std::string> roles, clip_shas;
    for (const cJSON* clip = clips->child; clip != nullptr; clip = clip->next) {
        if (!ExactKeys(clip, {"role", "assetVersionId", "sha256", "mediaType", "alpha"}) ||
            !UuidV4(Get(clip, "assetVersionId")) || !LowerHex64(Get(clip, "sha256")) ||
            !StringEquals(Get(clip, "mediaType"), "video/webm") || !cJSON_IsTrue(Get(clip, "alpha")) ||
            !cJSON_IsString(Get(clip, "role"))) return "robot clip";
        const std::string role = Get(clip, "role")->valuestring;
        if (std::find(std::begin(kClipRoles), std::end(kClipRoles), role) == std::end(kClipRoles)) return "robot role";
        roles.insert(role);
        clip_shas.insert(Get(clip, "sha256")->valuestring);
        assets->emplace(Get(clip, "assetVersionId")->valuestring,
                        JourneyAsset{Get(clip, "sha256")->valuestring, "video/webm", false, 0, 0});
    }
    if (roles.size() != 4 || clip_shas.size() != 4) return "robot clips must cover each role with unique bytes";
    if (const char* error = ScenePath(Get(journey, "scenePath"))) return error;
    const cJSON* context = Get(journey, "boundedContext");
    if (!ExactKeys(context, {"maxTurns"}) || !IntegerEquals(Get(context, "maxTurns"), 2)) return "boundedContext";
    const cJSON* steps = Get(journey, "steps");
    if (!cJSON_IsArray(steps) || cJSON_GetArraySize(steps) == 0) return "steps";
    const int count = cJSON_GetArraySize(steps);
    std::set<std::string> keys;
    int index = 0;
    for (const cJSON* step = steps->child; step != nullptr; step = step->next, ++index) {
        if (!ExactKeys(step, {"stepKey", "targetWord", "vietnameseMeanings", "relatedConcepts", "questionSeeds",
                              "teachingCopy", "expectedAnswer", "progress", "pronunciation", "contextTurns",
                              "teachingObject"})) return "step keys";
        const cJSON* key = Get(step, "stepKey");
        if (!cJSON_IsString(key) || !Slug(key->valuestring, SIZE_MAX)) return "stepKey";
        keys.insert(key->valuestring);
        const cJSON* copy = Get(step, "teachingCopy");
        if (!ExactKeys(copy, {"intro", "explanation", "prompt"}) || !String(Get(copy, "intro")) ||
            !String(Get(copy, "explanation")) || !String(Get(copy, "prompt"))) return "teachingCopy";
        if (!String(Get(step, "targetWord")) || !String(Get(step, "expectedAnswer"))) return "step words";
        for (const char* field : {"vietnameseMeanings", "relatedConcepts", "questionSeeds", "contextTurns"}) {
            if (!Strings(Get(step, field))) return "step text lists";
        }
        if (!cJSON_IsObject(Get(step, "pronunciation"))) return "pronunciation";
        const cJSON* progress = Get(step, "progress");
        if (!ExactKeys(progress, {"index", "count"}) || !IntegerEquals(Get(progress, "index"), index + 1) ||
            !IntegerEquals(Get(progress, "count"), count)) return "step progress";
        JourneyAsset teaching{};
        if (const char* error = SourceAsset(Get(step, "teachingObject"), "image/png", &teaching, &id)) return error;
        assets->emplace(id, teaching);
    }
    if (keys.size() != static_cast<std::size_t>(count)) return "stepKey values must be unique";
    if (assets->size() != static_cast<std::size_t>(1 + 4 + count)) return "assetVersionId values must be unique";
    *steps_out = static_cast<std::size_t>(count);
    return nullptr;
}

const char* Original(const cJSON* value, OriginalSourceOriginal* out, std::string* media_type) {
    if (!ExactKeys(value, {"assetVersionId", "sha256", "bytes", "mediaType", "container", "codec", "codecProfile",
                           "pixelFormat", "bitDepth", "alpha", "width", "height", "frameCount", "timeBase"}))
        return "original keys";
    if (!UuidV4(Get(value, "assetVersionId")) || !LowerHex64(Get(value, "sha256"))) return "original identity";
    std::uint64_t bytes = 0, width = 0, height = 0, frames = 0, num = 0, den = 0;
    if (!Integer(Get(value, "bytes"), 1, kOriginalSourceMaxBytes, &bytes)) return "original bytes";
    std::string tuple;
    for (const char* key : {"mediaType", "container", "codec", "codecProfile", "pixelFormat", "alpha"}) {
        const cJSON* item = Get(value, key);
        if (!cJSON_IsString(item)) return "original format";
        tuple += item->valuestring;
        tuple += '|';
    }
    if (tuple == "video/mp4|mp4|h264|constrained-baseline|yuv420p|none|" ||
        tuple == "video/mp4|mp4|h264|main|yuv420p|none|" || tuple == "video/mp4|mp4|h264|high|yuv420p|none|") {
        out->codec = OriginalSourceCodecId::kH264;
    } else if (tuple == "video/webm|webm|vp9|profile0|yuv420p|vp9-blockadditional|") {
        out->codec = OriginalSourceCodecId::kVp9Alpha;
    } else if (tuple == "image/png|png|png|none|rgba|straight-rgba|") {
        out->codec = OriginalSourceCodecId::kPng;
    } else {
        return "unsupported media/container/codec/profile/pixel/alpha tuple";
    }
    if (!IntegerEquals(Get(value, "bitDepth"), 8)) return "bitDepth";
    if (!Integer(Get(value, "width"), 1, kOriginalSourceMaxDimension, &width) ||
        !Integer(Get(value, "height"), 1, kOriginalSourceMaxDimension, &height)) return "original dimensions";
    if (!Integer(Get(value, "frameCount"), 1, kOriginalSourceMaxFrames, &frames)) return "frameCount";
    if ((out->codec == OriginalSourceCodecId::kPng) != (frames == 1)) return "frameCount does not match codec";
    const cJSON* time_base = Get(value, "timeBase");
    if (!ExactKeys(time_base, {"num", "den"}) ||
        !Integer(Get(time_base, "num"), 1, kOriginalSourceMaxTimeBase, &num) ||
        !Integer(Get(time_base, "den"), 1, kOriginalSourceMaxTimeBase, &den)) return "timeBase";
    out->asset_version_id = Get(value, "assetVersionId")->valuestring;
    out->sha256 = Get(value, "sha256")->valuestring;
    out->bytes = static_cast<std::uint32_t>(bytes);
    out->codec_profile = Get(value, "codecProfile")->valuestring;
    out->width = static_cast<std::uint32_t>(width);
    out->height = static_cast<std::uint32_t>(height);
    out->frame_count = static_cast<std::uint32_t>(frames);
    out->time_base_num = static_cast<std::uint32_t>(num);
    out->time_base_den = static_cast<std::uint32_t>(den);
    *media_type = Get(value, "mediaType")->valuestring;
    return nullptr;
}

std::string Capability(const OriginalSourceOriginal& original) {
    if (original.codec == OriginalSourceCodecId::kH264) return "decode.h264." + original.codec_profile + ".yuv420p8";
    if (original.codec == OriginalSourceCodecId::kVp9Alpha) return "decode.vp9.profile0.yuv420p8.alpha-blockadditional";
    return "decode.png.rgba8";
}

std::string Fingerprint(const OriginalSourceCommandInfo& command) {
    static const char* const kNames[] = {"prepare", "start", "pause", "resume", "stop", "cancel"};
    std::string print = kNames[static_cast<int>(command.command)];
    print += '|' + command.cue_id + '|';
    if (command.command == OriginalSourceCommandName::kPrepare) {
        print += command.scene_cache_key + '|' + command.scene_sha256 + '|' + std::to_string(command.scene_bytes);
    }
    print += '|' + command.stop_reason;
    return print;
}

}  // namespace

const char* ParseOriginalSourceScene(const cJSON* scene, OriginalSourceSceneInfo* out) {
    if (!ExactKeys(scene, {"contractVersion", "rendererVersion", "journey", "originals", "requiredCapabilities"}))
        return "scene keys";
    if (!StringEquals(Get(scene, "contractVersion"), kOriginalSourceSceneContract)) return "unsupported scene contract";
    if (!StringEquals(Get(scene, "rendererVersion"), kLessonRendererV6)) return "unsupported renderer version";
    std::map<std::string, JourneyAsset> expected;
    OriginalSourceSceneInfo info;
    if (const char* error = Journey(Get(scene, "journey"), &expected, &info.step_count)) return error;
    const cJSON* originals = Get(scene, "originals");
    if (!cJSON_IsArray(originals) || cJSON_GetArraySize(originals) == 0 ||
        static_cast<std::size_t>(cJSON_GetArraySize(originals)) > kOriginalSourceMaxOriginals)
        return "originals must be a bounded non-empty array";
    for (const cJSON* item = originals->child; item != nullptr; item = item->next) {
        OriginalSourceOriginal original;
        std::string media_type;
        if (const char* error = Original(item, &original, &media_type)) return error;
        if (!info.originals.empty() && info.originals.back().asset_version_id >= original.asset_version_id)
            return "originals must be strictly ordered by assetVersionId";
        const auto reference = expected.find(original.asset_version_id);
        if (reference == expected.end()) return "original is not referenced by the journey";
        if (reference->second.sha256 != original.sha256 || reference->second.media_type != media_type)
            return "original does not match its journey identity";
        if (reference->second.has_size && (reference->second.width != original.width ||
                                           reference->second.height != original.height))
            return "original dimensions differ from the journey";
        info.originals.push_back(std::move(original));
    }
    if (info.originals.size() != expected.size()) return "originals must cover every journey asset exactly once";
    std::set<std::string> derived{"scene.tvideo-journey.v1"};
    for (const auto& original : info.originals) derived.insert(Capability(original));
    const cJSON* required = Get(scene, "requiredCapabilities");
    if (!cJSON_IsArray(required) || static_cast<std::size_t>(cJSON_GetArraySize(required)) != derived.size())
        return "requiredCapabilities mismatch";
    auto expected_capability = derived.begin();  // std::set is byte-ordered, like the producer's sort.
    for (const cJSON* item = required->child; item != nullptr; item = item->next, ++expected_capability) {
        if (!StringEquals(item, expected_capability->c_str())) return "requiredCapabilities mismatch";
        info.required_capabilities.push_back(*expected_capability);
    }
    *out = std::move(info);
    return nullptr;
}

std::vector<std::string> MissingOriginalSourceCapabilities(const OriginalSourceSceneInfo& scene,
                                                            const std::vector<std::string>& advertised) {
    std::vector<std::string> missing;
    for (const auto& capability : scene.required_capabilities) {
        if (std::find(advertised.begin(), advertised.end(), capability) == advertised.end())
            missing.push_back(capability);
    }
    return missing;
}

const char* ParseOriginalSourceCommand(const char* frame_type, const cJSON* body, OriginalSourceCommandInfo* out) {
    if (frame_type == nullptr) return "frame type";
    const bool prepare = std::strcmp(frame_type, "lesson_prepare") == 0;
    const bool start = std::strcmp(frame_type, "lesson_start") == 0;
    const bool stop = std::strcmp(frame_type, "lesson_stop") == 0;
    const bool control = std::strcmp(frame_type, "lesson_cinematic_control") == 0;
    if (!prepare && !start && !stop && !control) return "unsupported frame type";
    const cJSON* command = body;
    if (!control) {
        const bool keys_ok = stop ? ExactKeys(body, {"reason", "cinematicPhase"}) : ExactKeys(body, {"cinematicPhase"});
        if (!keys_ok) return "body must wrap cinematicPhase";
        command = Get(body, "cinematicPhase");
    }
    std::string stop_reason;
    if (stop) {
        const cJSON* reason = Get(body, "reason");
        if (!StringEquals(reason, "COMPLETED") && !StringEquals(reason, "CANCELLED") && !StringEquals(reason, "FAILED"))
            return "lesson_stop reason must be COMPLETED, CANCELLED or FAILED";
        stop_reason = reason->valuestring;
    }
    if (!cJSON_IsObject(command)) return "command must be an object";
    const cJSON* name = Get(command, "command");
    if (!cJSON_IsString(name)) return "command name";
    const std::string verb = name->valuestring;
    OriginalSourceCommandInfo info;
    if (prepare && verb == "prepare") info.command = OriginalSourceCommandName::kPrepare;
    else if ((start || control) && verb == "start") info.command = OriginalSourceCommandName::kStart;
    else if (stop && verb == "stop") info.command = OriginalSourceCommandName::kStop;
    else if (control && verb == "pause") info.command = OriginalSourceCommandName::kPause;
    else if (control && verb == "resume") info.command = OriginalSourceCommandName::kResume;
    else if (control && verb == "cancel") info.command = OriginalSourceCommandName::kCancel;
    else return "frame cannot carry this command";
    bool keys_ok = false;
    switch (info.command) {
        case OriginalSourceCommandName::kPrepare:
            keys_ok = ExactKeys(command, {"command", "cueId", "commandSequenceId", "scene"}); break;
        case OriginalSourceCommandName::kResume:
            keys_ok = ExactKeys(command, {"command", "cueId", "commandSequenceId", "clockRebaseSequenceId"}); break;
        case OriginalSourceCommandName::kCancel:
            keys_ok = ExactKeys(command, {"command", "cueId", "commandSequenceId", "reason"}); break;
        default:
            keys_ok = ExactKeys(command, {"command", "cueId", "commandSequenceId"}); break;
    }
    if (!keys_ok) return "command keys";
    const cJSON* cue = Get(command, "cueId");
    if (!cJSON_IsString(cue) || !Slug(cue->valuestring, kMaxCueBytes)) return "cueId must be a canonical slug";
    info.cue_id = cue->valuestring;
    info.stop_reason = stop_reason;
    if (!Integer(Get(command, "commandSequenceId"), 1, kMaxSafeInteger, &info.command_sequence_id))
        return "commandSequenceId";
    if (info.command == OriginalSourceCommandName::kResume) {
        std::uint64_t rebase = 0;
        if (!Integer(Get(command, "clockRebaseSequenceId"), 1, kMaxSafeInteger, &rebase) ||
            rebase != info.command_sequence_id) return "clockRebaseSequenceId must equal commandSequenceId";
    }
    if (info.command == OriginalSourceCommandName::kCancel) {
        const cJSON* reason = Get(command, "reason");
        if (!cJSON_IsString(reason) || std::strlen(reason->valuestring) > kMaxReasonBytes) return "cancel reason";
        bool blank = true;
        for (const char* c = reason->valuestring; *c != '\0'; ++c) {
            if (!std::strchr(" \t\n\v\f\r", *c)) { blank = false; break; }
        }
        if (blank) return "cancel reason must be non-blank";
        info.reason = reason->valuestring;
    }
    if (info.command == OriginalSourceCommandName::kPrepare) {
        const cJSON* scene = Get(command, "scene");
        std::uint64_t bytes = 0;
        if (!ExactKeys(scene, {"cacheKey", "sha256", "bytes"}) || !cJSON_IsString(Get(scene, "cacheKey")) ||
            !IsCanonicalLessonCacheKey(Get(scene, "cacheKey")->valuestring) || !LowerHex64(Get(scene, "sha256")) ||
            !Integer(Get(scene, "bytes"), 1, kOriginalSourceSceneMaxBytes, &bytes)) return "scene reference";
        info.scene_cache_key = Get(scene, "cacheKey")->valuestring;
        info.scene_sha256 = Get(scene, "sha256")->valuestring;
        info.scene_bytes = static_cast<std::uint32_t>(bytes);
    }
    *out = std::move(info);
    return nullptr;
}

OriginalSourceVerdict ApplyOriginalSourceCommand(OriginalSourceControlState* state,
                                                 const OriginalSourceCommandInfo& command) {
    const std::string print = Fingerprint(command);
    if (state->last_sequence != 0 && command.command_sequence_id == state->last_sequence) {
        return state->last_fingerprint == print ? OriginalSourceVerdict::kReplayed : OriginalSourceVerdict::kStale;
    }
    if (command.command_sequence_id < state->last_sequence) return OriginalSourceVerdict::kStale;
    if (command.command != OriginalSourceCommandName::kPrepare && command.cue_id != state->cue_id)
        return OriginalSourceVerdict::kStale;
    using Phase = OriginalSourceRendererPhase;
    using Name = OriginalSourceCommandName;
    if ((command.command == Name::kStart && state->phase != Phase::kPrepared) ||
        (command.command == Name::kPause && state->phase != Phase::kRunning) ||
        (command.command == Name::kResume && state->phase != Phase::kPaused))
        return OriginalSourceVerdict::kInvalidState;
    switch (command.command) {
        case Name::kPrepare: state->phase = Phase::kPrepared; break;
        case Name::kStart: case Name::kResume: state->phase = Phase::kRunning; break;
        case Name::kPause: state->phase = Phase::kPaused; break;
        case Name::kStop: case Name::kCancel: state->phase = Phase::kIdle; break;
    }
    state->cue_id = command.cue_id;
    state->last_sequence = command.command_sequence_id;
    state->last_fingerprint = print;
    return OriginalSourceVerdict::kApplied;
}

}  // namespace tbot
