#include "lesson_tvideo_raster_canvas.h"
#include "lesson_original_source_profile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace tbot {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kFlattenTolerance = 0.02;  // device pixels of chord error
constexpr int kSubScanlines = 16;
constexpr double kMiterLimit = 10;

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

double Overlap(double a0, double a1, double b0, double b1) { return std::max(0.0, std::min(a1, b1) - std::max(a0, b0)); }

struct Edge {
    double x0, y0, x1, y1;
    int direction;
};

}  // namespace

bool ParseTVideoColor(const char* style, TVideoRasterCanvas::Color* out) {
    if (style == nullptr) return false;
    if (style[0] == '#' && std::strlen(style) == 7) {
        int value[6];
        for (int index = 0; index < 6; ++index) {
            value[index] = HexDigit(style[index + 1]);
            if (value[index] < 0) return false;
        }
        *out = {static_cast<float>(value[0] * 16 + value[1]), static_cast<float>(value[2] * 16 + value[3]),
                static_cast<float>(value[4] * 16 + value[5]), 1};
        return true;
    }
    if (std::strncmp(style, "rgba(", 5) == 0) {
        double parts[4];
        const char* cursor = style + 5;
        for (int index = 0; index < 4; ++index) {
            char* end = nullptr;
            parts[index] = std::strtod(cursor, &end);
            if (end == cursor) return false;
            cursor = end;
            while (*cursor == ' ') ++cursor;
            if (*cursor != (index == 3 ? ')' : ',')) return false;
            ++cursor;
        }
        if (*cursor != '\0') return false;
        for (int index = 0; index < 3; ++index) {
            if (parts[index] < 0 || parts[index] > 255) return false;
        }
        if (!(parts[3] >= 0 && parts[3] <= 1)) return false;
        *out = {static_cast<float>(parts[0]), static_cast<float>(parts[1]), static_cast<float>(parts[2]),
                static_cast<float>(parts[3])};
        return true;
    }
    return false;
}

TVideoRasterCanvas::TVideoRasterCanvas(std::uint8_t* rgb, TVideoTextRenderer* text) : rgb_(rgb), text_(text) {}

void TVideoRasterCanvas::SetMedia(TVideoMedia media, const ComposeSource* source) {
    media_[static_cast<std::size_t>(media)] = source;
}

void TVideoRasterCanvas::SetFillStyle(const char* style) { ParseTVideoColor(style, &state_.fill); }
void TVideoRasterCanvas::SetStrokeStyle(const char* style) { ParseTVideoColor(style, &state_.stroke); }
void TVideoRasterCanvas::SetGlobalAlpha(double alpha) {
    // Canvas ignores out-of-range and non-finite values.
    if (alpha >= 0 && alpha <= 1) state_.alpha = alpha;
}
void TVideoRasterCanvas::SetLineWidth(double width) {
    if (std::isfinite(width) && width > 0) state_.line_width = width;
}
void TVideoRasterCanvas::SetFont(const char* font) {
    const char* px = font != nullptr ? std::strstr(font, "px") : nullptr;
    if (px == nullptr) return;
    const char* digits = px;
    while (digits > font && digits[-1] >= '0' && digits[-1] <= '9') --digits;
    if (digits < px) state_.font_px = std::atoi(std::string(digits, px).c_str());
}
void TVideoRasterCanvas::SetTextAlign(const char* align) {
    state_.align_center = align != nullptr && std::strcmp(align, "center") == 0;
}
void TVideoRasterCanvas::SetTextBaseline(const char*) {}  // the canonical renderer only uses alphabetic

void TVideoRasterCanvas::Save() { stack_.push_back(state_); }
void TVideoRasterCanvas::Restore() {
    if (stack_.empty()) return;
    state_ = stack_.back();
    stack_.pop_back();
}

void TVideoRasterCanvas::Translate(double x, double y) {
    Matrix& m = state_.matrix;
    m.e += m.a * x + m.c * y;
    m.f += m.b * x + m.d * y;
}
void TVideoRasterCanvas::Scale(double x, double y) {
    Matrix& m = state_.matrix;
    m.a *= x;
    m.b *= x;
    m.c *= y;
    m.d *= y;
}
void TVideoRasterCanvas::Rotate(double radians) {
    Matrix& m = state_.matrix;
    const double cosine = std::cos(radians), sine = std::sin(radians);
    const Matrix old = m;
    m.a = old.a * cosine + old.c * sine;
    m.b = old.b * cosine + old.d * sine;
    m.c = old.c * cosine - old.a * sine;
    m.d = old.d * cosine - old.b * sine;
}

TVideoRasterCanvas::Point TVideoRasterCanvas::Transform(Point point) const {
    const Matrix& m = state_.matrix;
    return {m.a * point.x + m.c * point.y + m.e, m.b * point.x + m.d * point.y + m.f};
}

void TVideoRasterCanvas::DrawMedia(TVideoMedia media, double x, double y, double width, double height) {
    OriginalSourceProfileScope timed(&OriginalSourceProfileCounters().draw_media_us);
    const ComposeSource* source = media_[static_cast<std::size_t>(media)];
    if (source == nullptr || source->width <= 0 || source->height <= 0) return;
    const Matrix& m = state_.matrix;
    if (m.b != 0 || m.c != 0 || m.a <= 0 || m.d <= 0) {
        unsupported_ = "media under rotation or mirroring";
        return;
    }
    const ClipRect& clip = state_.clip;
    if (clip.x0 > 0 || clip.y0 > 0 || clip.x1 < kTVideoStageWidth || clip.y1 < kTVideoStageHeight) {
        unsupported_ = "media under a clip";
        return;
    }
    const Point origin = Transform({x, y});
    DrawComposeSource(*source, origin.x, origin.y, width * m.a, height * m.d, state_.alpha, rgb_);
}

void TVideoRasterCanvas::BeginPath() { path_.clear(); }
void TVideoRasterCanvas::ClosePath() {
    if (!path_.empty() && !path_.back().points.empty()) {
        path_.back().closed = true;
        // A later segment starts a new subpath at the closed subpath's start.
        path_.push_back({{path_.back().points.front()}, false});
    }
}

TVideoRasterCanvas::SubPath& TVideoRasterCanvas::Current() {
    if (path_.empty()) path_.push_back({});
    return path_.back();
}

void TVideoRasterCanvas::MoveTo(double x, double y) { path_.push_back({{{x, y}}, false}); }
void TVideoRasterCanvas::LineTo(double x, double y) { Current().points.push_back({x, y}); }

void TVideoRasterCanvas::Rect(double x, double y, double width, double height) {
    path_.push_back({{{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}}, true});
    path_.push_back({{{x, y}}, false});
}

void TVideoRasterCanvas::AppendArc(double x, double y, double radius_x, double radius_y, double start, double end) {
    double sweep = end - start;
    if (sweep >= kTwoPi) {
        sweep = kTwoPi;
    } else if (sweep < 0) {
        sweep = std::fmod(sweep, kTwoPi) + kTwoPi;
    }
    const Matrix& m = state_.matrix;
    const double device_scale = std::sqrt(std::fabs(m.a * m.d - m.b * m.c));
    const double radius = std::max(std::fabs(radius_x), std::fabs(radius_y)) * device_scale;
    int segments = 1;
    if (radius > kFlattenTolerance) {
        const double step = 2 * std::acos(std::max(-1.0, 1 - kFlattenTolerance / radius));
        segments = static_cast<int>(std::ceil(sweep / step));
    }
    segments = std::min(512, std::max(4, segments));
    SubPath& sub = Current();
    for (int index = 0; index <= segments; ++index) {
        const double angle = start + sweep * index / segments;
        sub.points.push_back({x + radius_x * std::cos(angle), y + radius_y * std::sin(angle)});
    }
}

void TVideoRasterCanvas::Arc(double x, double y, double radius, double start, double end) {
    if (!(radius >= 0)) return;
    AppendArc(x, y, radius, radius, start, end);
}

void TVideoRasterCanvas::Ellipse(double x, double y, double radius_x, double radius_y, double rotation,
                                 double start, double end) {
    if (!(radius_x >= 0 && radius_y >= 0)) return;
    if (rotation == 0) {
        AppendArc(x, y, radius_x, radius_y, start, end);
        return;
    }
    const std::size_t from = Current().points.size();
    AppendArc(0, 0, radius_x, radius_y, start, end);
    const double cosine = std::cos(rotation), sine = std::sin(rotation);
    auto& points = Current().points;
    for (std::size_t index = from; index < points.size(); ++index) {
        const Point p = points[index];
        points[index] = {x + p.x * cosine - p.y * sine, y + p.x * sine + p.y * cosine};
    }
}

void TVideoRasterCanvas::RoundRect(double x, double y, double width, double height, double radius) {
    if (!(width > 0 && height > 0 && radius >= 0)) return;
    // Canvas scales all radii down together when adjacent corners would overlap.
    if (radius > 0) radius *= std::min(1.0, std::min(width / (2 * radius), height / (2 * radius)));
    path_.push_back({});
    constexpr double kHalfPi = kTwoPi / 4;
    AppendArc(x + width - radius, y + radius, radius, radius, -kHalfPi, 0);
    AppendArc(x + width - radius, y + height - radius, radius, radius, 0, kHalfPi);
    AppendArc(x + radius, y + height - radius, radius, radius, kHalfPi, 2 * kHalfPi);
    AppendArc(x + radius, y + radius, radius, radius, 2 * kHalfPi, 3 * kHalfPi);
    path_.back().closed = true;
    path_.push_back({{{x, y}}, false});
}

void TVideoRasterCanvas::Clip() {
    std::vector<Point> corners;
    for (const SubPath& sub : path_) {
        if (sub.points.size() <= 1 && !sub.closed) continue;
        if (!corners.empty() || sub.points.size() != 4) {
            unsupported_ = "non-rectangular clip";
            return;
        }
        for (const Point& point : sub.points) corners.push_back(Transform(point));
    }
    if (corners.size() != 4) {
        unsupported_ = "non-rectangular clip";
        return;
    }
    const bool axis_aligned = (corners[0].y == corners[1].y && corners[1].x == corners[2].x &&
                               corners[2].y == corners[3].y && corners[3].x == corners[0].x);
    if (!axis_aligned) {
        unsupported_ = "non-rectangular clip";
        return;
    }
    ClipRect& clip = state_.clip;
    clip.x0 = std::max(clip.x0, std::min(corners[0].x, corners[2].x));
    clip.x1 = std::min(clip.x1, std::max(corners[0].x, corners[2].x));
    clip.y0 = std::max(clip.y0, std::min(corners[0].y, corners[2].y));
    clip.y1 = std::min(clip.y1, std::max(corners[0].y, corners[2].y));
}

void TVideoRasterCanvas::Blend(int x, int y, float coverage, const Color& color) {
    const ClipRect& clip = state_.clip;
    coverage *= static_cast<float>(Overlap(x, x + 1, clip.x0, clip.x1) * Overlap(y, y + 1, clip.y0, clip.y1));
    const float alpha = std::min(1.0f, coverage) * color.a * static_cast<float>(state_.alpha);
    if (alpha <= 0) return;
    std::uint8_t* pixel = rgb_ + (y * kTVideoStageWidth + x) * 3;
    pixel[0] = static_cast<std::uint8_t>(RoundPixel(color.r * alpha + pixel[0] * (1 - alpha)));
    pixel[1] = static_cast<std::uint8_t>(RoundPixel(color.g * alpha + pixel[1] * (1 - alpha)));
    pixel[2] = static_cast<std::uint8_t>(RoundPixel(color.b * alpha + pixel[2] * (1 - alpha)));
}

void TVideoRasterCanvas::FillPolygons(const std::vector<std::vector<Point>>& polygons, const Color& color) {
    OriginalSourceProfileScope timed(&OriginalSourceProfileCounters().fill_us);
    std::vector<Edge> edges;
    double min_x = 1e30, max_x = -1e30, min_y = 1e30, max_y = -1e30;
    for (const auto& polygon : polygons) {
        for (std::size_t index = 0; index < polygon.size(); ++index) {
            const Point a = polygon[index], b = polygon[(index + 1) % polygon.size()];
            if (!std::isfinite(a.x) || !std::isfinite(a.y)) return;
            min_x = std::min(min_x, a.x);
            max_x = std::max(max_x, a.x);
            min_y = std::min(min_y, a.y);
            max_y = std::max(max_y, a.y);
            if (a.y == b.y) continue;
            edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y, 1} : Edge{b.x, b.y, a.x, a.y, -1});
        }
    }
    if (edges.empty()) return;
    const ClipRect& clip = state_.clip;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::max(min_x, clip.x0))));
    const int x1 = std::min(kTVideoStageWidth, static_cast<int>(std::ceil(std::min(max_x, clip.x1))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::max(min_y, clip.y0))));
    const int y1 = std::min(kTVideoStageHeight, static_cast<int>(std::ceil(std::min(max_y, clip.y1))));
    if (x0 >= x1 || y0 >= y1) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    const int columns = x1 - x0;
    std::vector<float> coverage(static_cast<std::size_t>(columns));
    std::vector<const Edge*> active;
    std::vector<std::pair<double, int>> crossings;
    std::size_t next = 0;
    const float full_sub_coverage = static_cast<float>(1.0 / kSubScanlines);
    // Clip overlap per column, once per fill instead of per pixel (same values).
    std::vector<double> column_clip(static_cast<std::size_t>(columns));
    std::vector<std::uint8_t> column_unclipped(static_cast<std::size_t>(columns));
    for (int px = x0; px < x1; ++px) {
        column_clip[px - x0] = Overlap(px, px + 1, clip.x0, clip.x1);
        column_unclipped[px - x0] = column_clip[px - x0] == 1.0;
    }
    const float layer_alpha = static_cast<float>(state_.alpha);
    // Fully covered, unclipped pixels share one alpha; their blended byte depends only on
    // the byte below. The table holds exactly the per-pixel expression's results.
    std::uint8_t interior[3][256];
    bool interior_ready = false;
    float interior_alpha = 0;
    const auto BuildInterior = [&]() {
        const float covered = 1.0f;
        interior_alpha = std::min(1.0f, covered) * color.a * layer_alpha;
        for (int value = 0; value < 256; ++value) {
            const std::uint8_t below = static_cast<std::uint8_t>(value);
            const float alpha = interior_alpha;
            interior[0][value] = static_cast<std::uint8_t>(RoundPixel(color.r * alpha + below * (1 - alpha)));
            interior[1][value] = static_cast<std::uint8_t>(RoundPixel(color.g * alpha + below * (1 - alpha)));
            interior[2][value] = static_cast<std::uint8_t>(RoundPixel(color.b * alpha + below * (1 - alpha)));
        }
        interior_ready = true;
    };
    // Per row, a pixel's coverage is the float sum, in sub-scanline order, of 1/16 for each
    // sub-scanline whose span covers it fully and covered/16 where a span edge crosses it.
    // Sums of 1/16 alone are exact (n/16), so pixels never crossed by an edge take n/16 from
    // a difference array; only edge-crossed pixels replay the ordered float sum. Same values
    // as accumulating every pixel on every sub-scanline, without the 16 x width work.
    struct Span {
        double a, b;
        int lo, full_begin, full_end, end;
    };
    std::vector<Span> spans;
    std::vector<int> full_delta(static_cast<std::size_t>(columns) + 1);
    std::vector<std::uint8_t> crossed(static_cast<std::size_t>(columns));
    std::vector<int> crossed_columns;
    for (int row = y0; row < y1; ++row) {
        spans.clear();
        std::fill(full_delta.begin(), full_delta.end(), 0);
        for (int sub = 0; sub < kSubScanlines; ++sub) {
            const double sample_y = row + (sub + 0.5) / kSubScanlines;
            while (next < edges.size() && edges[next].y0 <= sample_y) active.push_back(&edges[next++]);
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&](const Edge* edge) { return edge->y1 <= sample_y; }),
                         active.end());
            crossings.clear();
            for (const Edge* edge : active) {
                if (edge->y0 > sample_y) continue;
                const double t = (sample_y - edge->y0) / (edge->y1 - edge->y0);
                crossings.push_back({edge->x0 + t * (edge->x1 - edge->x0), edge->direction});
            }
            std::sort(crossings.begin(), crossings.end());
            int winding = 0;
            double span_start = 0;
            for (const auto& crossing : crossings) {
                const int before = winding;
                winding += crossing.second;
                if (before == 0 && winding != 0) {
                    span_start = crossing.first;
                } else if (before != 0 && winding == 0) {
                    const double a = std::max(span_start, static_cast<double>(x0));
                    const double b = std::min(crossing.first, static_cast<double>(x1));
                    // Integer bounds, once per span (the ESP32-S3 emulates double math):
                    // px < b <=> px < ceil(b); pixels in [ceil(a), floor(b)) overlap the
                    // span by exactly 1.0, as Overlap() would return.
                    const int end = std::min(x1, static_cast<int>(std::ceil(b)));
                    const int lo = static_cast<int>(std::floor(a));
                    if (lo >= end) continue;
                    const Span span{a, b, lo, static_cast<int>(std::ceil(a)), static_cast<int>(std::floor(b)), end};
                    spans.push_back(span);
                    const int full_lo = std::max(span.lo, span.full_begin);
                    const int full_hi = std::min(span.end, span.full_end);
                    if (full_lo < full_hi) {
                        ++full_delta[full_lo - x0];
                        --full_delta[full_hi - x0];
                    }
                }
            }
        }
        if (spans.empty()) continue;
        // Columns an edge crosses on some sub-scanline: [lo, full) and [full_end, end).
        crossed_columns.clear();
        for (const Span& span : spans) {
            for (int px = span.lo; px < span.end; ++px) {
                if (px >= span.full_begin && px < span.full_end) {
                    px = std::max(px, span.full_end - 1);
                    continue;
                }
                if (!crossed[px - x0]) {
                    crossed[px - x0] = 1;
                    crossed_columns.push_back(px);
                }
            }
        }
        bool any = false;
        int full_count = 0;
        for (int column = 0; column < columns; ++column) {
            full_count += full_delta[column];
            coverage[column] = static_cast<float>(full_count) * full_sub_coverage;
            any = any || full_count > 0;
        }
        for (const int px : crossed_columns) {
            float sum = 0.0f;
            for (const Span& span : spans) {  // sub-scanline order, then span order
                if (px < span.lo || px >= span.end) continue;
                if (px >= span.full_begin && px < span.full_end) {
                    sum += full_sub_coverage;
                    any = true;
                    continue;
                }
                const double covered = Overlap(span.a, span.b, px, px + 1);
                if (covered > 0) {
                    sum += static_cast<float>(covered / kSubScanlines);
                    any = true;
                }
            }
            coverage[px - x0] = sum;
            crossed[px - x0] = 0;
        }
        if (!any) continue;
        const double row_clip = Overlap(row, row + 1, clip.y0, clip.y1);
        const bool row_unclipped = row_clip == 1.0;
        std::uint8_t* pixels = rgb_ + row * kTVideoStageWidth * 3;
        for (int px = x0; px < x1; ++px) {
            float covered = coverage[px - x0];
            if (covered <= 0) continue;
            std::uint8_t* pixel = pixels + px * 3;
            const bool unclipped = row_unclipped && column_unclipped[px - x0];
            if (covered == 1.0f && unclipped) {
                // Fully covered, unclipped: the alpha of every such pixel is interior_alpha,
                // so the blend is the per-byte table built from the same expression.
                if (!interior_ready) BuildInterior();
                if (interior_alpha <= 0) continue;
                pixel[0] = interior[0][pixel[0]];
                pixel[1] = interior[1][pixel[1]];
                pixel[2] = interior[2][pixel[2]];
                continue;
            }
            // Same arithmetic as Blend(): a clip overlap of exactly 1.0 x 1.0 leaves
            // the coverage unchanged, so only clip-edge pixels multiply in double.
            if (!unclipped) covered *= static_cast<float>(column_clip[px - x0] * row_clip);
            const float alpha = std::min(1.0f, covered) * color.a * layer_alpha;
            if (alpha <= 0) continue;
            pixel[0] = static_cast<std::uint8_t>(RoundPixel(color.r * alpha + pixel[0] * (1 - alpha)));
            pixel[1] = static_cast<std::uint8_t>(RoundPixel(color.g * alpha + pixel[1] * (1 - alpha)));
            pixel[2] = static_cast<std::uint8_t>(RoundPixel(color.b * alpha + pixel[2] * (1 - alpha)));
        }
    }
}

void TVideoRasterCanvas::Fill() {
    std::vector<std::vector<Point>> polygons;
    for (const SubPath& sub : path_) {
        if (sub.points.size() < 3) continue;
        std::vector<Point> device;
        for (const Point& point : sub.points) device.push_back(Transform(point));
        polygons.push_back(std::move(device));
    }
    FillPolygons(polygons, state_.fill);
}

void TVideoRasterCanvas::Stroke() {
    const double half = state_.line_width / 2;
    std::vector<std::vector<Point>> polygons;
    for (const SubPath& sub : path_) {
        std::vector<Point> points;
        for (const Point& point : sub.points) {
            if (points.empty() || std::hypot(point.x - points.back().x, point.y - points.back().y) > 1e-9) {
                points.push_back(point);
            }
        }
        bool closed = sub.closed;
        if (points.size() > 2 &&
            std::hypot(points.front().x - points.back().x, points.front().y - points.back().y) <= 1e-9) {
            points.pop_back();
            closed = true;  // a full arc ends where it starts
        }
        const std::size_t count = points.size();
        if (count < 2) continue;
        const std::size_t segments = closed ? count : count - 1;
        std::vector<Point> normals(segments);
        for (std::size_t index = 0; index < segments; ++index) {
            const Point a = points[index], b = points[(index + 1) % count];
            const double length = std::hypot(b.x - a.x, b.y - a.y);
            normals[index] = {-(b.y - a.y) / length, (b.x - a.x) / length};
        }
        std::vector<Point> left, right;
        for (std::size_t index = 0; index < count; ++index) {
            const bool has_in = closed || index > 0, has_out = closed || index + 1 < count;
            const Point p = points[index];
            const Point n_in = has_in ? normals[(index + segments - 1) % segments] : normals[0];
            const Point n_out = has_out ? normals[index % segments] : n_in;
            const double cosine = n_in.x * n_out.x + n_in.y * n_out.y;
            const double miter = 1 + cosine;
            if (miter > 1e-9 && 2 / miter <= kMiterLimit * kMiterLimit) {
                const Point m{(n_in.x + n_out.x) / miter, (n_in.y + n_out.y) / miter};
                left.push_back({p.x + m.x * half, p.y + m.y * half});
                right.push_back({p.x - m.x * half, p.y - m.y * half});
            } else {
                left.push_back({p.x + n_in.x * half, p.y + n_in.y * half});
                left.push_back({p.x + n_out.x * half, p.y + n_out.y * half});
                right.push_back({p.x - n_in.x * half, p.y - n_in.y * half});
                right.push_back({p.x - n_out.x * half, p.y - n_out.y * half});
            }
        }
        for (auto* outline : {&left, &right}) {
            for (Point& point : *outline) point = Transform(point);
        }
        std::reverse(right.begin(), right.end());
        if (closed) {
            polygons.push_back(std::move(left));
            polygons.push_back(std::move(right));
        } else {
            left.insert(left.end(), right.begin(), right.end());
            polygons.push_back(std::move(left));
        }
    }
    FillPolygons(polygons, state_.stroke);
}

void TVideoRasterCanvas::FillRect(double x, double y, double width, double height) {
    FillPolygons({{Transform({x, y}), Transform({x + width, y}), Transform({x + width, y + height}),
                   Transform({x, y + height})}},
                 state_.fill);
}

double TVideoRasterCanvas::MeasureText(const std::string& text) {
    return text_ != nullptr ? text_->Measure(text, state_.font_px) : 0;
}

void TVideoRasterCanvas::FillText(const std::string& text, double x, double y) {
    OriginalSourceProfileScope timed(&OriginalSourceProfileCounters().text_us);
    if (text_ == nullptr) return;
    const Matrix& m = state_.matrix;
    if (m.b != 0 || m.c != 0 || m.a <= 0 || m.a != m.d) {
        unsupported_ = "text under rotation or non-uniform scale";
        return;
    }
    const double left = state_.align_center ? x - text_->Measure(text, state_.font_px) / 2 : x;
    const Point origin = Transform({left, y});
    const Color color = state_.fill;
    text_->Render(text, state_.font_px, m.a, origin.x, origin.y, [&](int px, int py, float coverage) {
        if (px >= 0 && py >= 0 && px < kTVideoStageWidth && py < kTVideoStageHeight) Blend(px, py, coverage, color);
    });
}

}  // namespace tbot
