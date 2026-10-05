#include "lesson_tvideo_painter.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

namespace tbot {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr char kInk[] = "#17324d";
constexpr char kLine[] = "#efe6d6";
constexpr char kSun[] = "#ffd166";
constexpr char kMint[] = "#79d8bd";
constexpr char kGrape[] = "#b39ddb";
constexpr char kSky[] = "#5bb8e6";
// tvideo-journey.layout.ts puff particles and tvideo-journey.preset.ts confetti colours.
constexpr const char* kPuffColors[] = {"#ffd166", "#79d8bd", "#ff8a6b", "#ffffff", "#b39ddb"};
constexpr const char* kConfettiColors[] = {"#ffd166", "#ff8a6b", "#79d8bd", "#b39ddb", "#5bb8e6", "#ffffff"};
constexpr const char* kPromptFonts[] = {"700 16px \"Noto Sans\"", "700 15px \"Noto Sans\"", "700 14px \"Noto Sans\"",
                                        "700 13px \"Noto Sans\""};
constexpr int kPromptFontSizes[] = {16, 15, 14, 13};
constexpr double kPromptMaxWidth = 156;
constexpr std::size_t kPromptMaxLines = 4;

TVideoMedia RobotMedia(TVideoClipRole role) {
    switch (role) {
        case TVideoClipRole::kFlight: return TVideoMedia::kRobotFlight;
        case TVideoClipRole::kWalking: return TVideoMedia::kRobotWalking;
        case TVideoClipRole::kGreetingTeaching: return TVideoMedia::kRobotGreetingTeaching;
        case TVideoClipRole::kCelebration: return TVideoMedia::kRobotCelebration;
    }
    return TVideoMedia::kRobotFlight;
}

void RoundedPath(TVideoCanvas* canvas, double x, double y, double width, double height, double radius) {
    canvas->BeginPath();
    canvas->RoundRect(x, y, width, height, radius);
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::vector<std::string> SplitWords(const std::string& input) {
    std::vector<std::string> words;
    std::size_t index = 0;
    while (index < input.size()) {
        while (index < input.size() && IsSpace(input[index])) ++index;
        const std::size_t start = index;
        while (index < input.size() && !IsSpace(input[index])) ++index;
        if (index > start) words.push_back(input.substr(start, index - start));
    }
    return words;
}

bool WrapPromptLines(TVideoCanvas* canvas, const std::string& input, std::vector<std::string>* lines) {
    lines->clear();
    for (const std::string& word : SplitWords(input)) {
        if (canvas->MeasureText(word) > kPromptMaxWidth) return false;
        if (lines->empty() || canvas->MeasureText(lines->back() + " " + word) > kPromptMaxWidth) {
            if (lines->size() == kPromptMaxLines) return false;
            lines->push_back(word);
        } else {
            lines->back() += " " + word;
        }
    }
    return true;
}

void PaintWordPill(TVideoCanvas* canvas, const std::string& label, const TVideoFrameLayout& layout) {
    canvas->Save();
    canvas->SetGlobalAlpha(layout.word_pill.opacity);
    canvas->SetFont("700 18px \"Noto Sans\"");
    canvas->SetTextAlign("center");
    canvas->SetTextBaseline("alphabetic");
    const double width = std::max(58.0, canvas->MeasureText(label) + 24);
    canvas->SetFillStyle("#ffffff");
    RoundedPath(canvas, layout.word_pill.center_x - width / 2, layout.word_pill.top, width, layout.word_pill.height, 14);
    canvas->Fill();
    canvas->SetStrokeStyle(kLine);
    canvas->SetLineWidth(1.5);
    canvas->Stroke();
    canvas->SetFillStyle("#5b462d");
    canvas->FillText(label, layout.word_pill.center_x, layout.object.center_y + 70.5);
    canvas->Restore();
    canvas->SetGlobalAlpha(1);
}

const char* PaintCard(TVideoCanvas* canvas, const TVideoFrameState& state, const TVideoFrameLayout& layout,
                      const TVideoCopy& copy) {
    const auto& card = layout.card;
    canvas->Save();
    canvas->Translate(card.offset_x, card.offset_y);
    canvas->SetGlobalAlpha(card.opacity);
    canvas->Save();
    canvas->SetFillStyle("#ffffff");
    RoundedPath(canvas, card.x, card.y, card.width, card.height, card.radius);
    canvas->Fill();
    canvas->SetStrokeStyle(kLine);
    canvas->SetLineWidth(1.5);
    canvas->Stroke();
    if (card.cue == TVideoCardCue::kListening) {
        canvas->SetStrokeStyle("rgba(121,216,189,.6)");
        canvas->SetLineWidth(card.ring_line_width);
        RoundedPath(canvas, card.x, card.y, card.width, card.height, card.radius);
        canvas->Stroke();
        canvas->SetFillStyle(kMint);
        canvas->BeginPath();
        canvas->Arc(195, 51, 5, 0, kPi * 2);
        canvas->Fill();
    } else if (card.cue == TVideoCardCue::kThinking) {
        const double raw = std::isfinite(state.card.gentle_pulse) ? state.card.gentle_pulse : 0;
        const double pulse = std::min(1.0, std::max(0.0, raw));
        canvas->SetStrokeStyle(kGrape);
        canvas->SetLineWidth(card.ring_line_width);
        RoundedPath(canvas, card.x, card.y, card.width, card.height, card.radius);
        canvas->Stroke();
        for (int index = 0; index < 3; ++index) {
            canvas->SetGlobalAlpha(.45 + pulse * .4 - index * .08);
            canvas->SetFillStyle(kGrape);
            canvas->BeginPath();
            canvas->Arc(180 + index * 10, 51 - index * 3, 3 + index, 0, kPi * 2);
            canvas->Fill();
        }
    } else if (card.cue == TVideoCardCue::kRetry) {
        const double cue_x = 190 + state.card.retry_level * 2;
        canvas->SetStrokeStyle("#f4a261");
        canvas->SetLineWidth(card.ring_line_width);
        RoundedPath(canvas, card.x, card.y, card.width, card.height, card.radius);
        canvas->Stroke();
        for (int index = 0; index < state.card.retry_level; ++index) {
            canvas->BeginPath();
            canvas->Arc(cue_x, 51, 5 + index * 6, kPi * 1.08, kPi * 1.92);
            canvas->Stroke();
        }
    } else if (card.cue == TVideoCardCue::kWordTransition) {
        canvas->SetStrokeStyle(kSky);
        canvas->SetLineWidth(card.ring_line_width);
        RoundedPath(canvas, card.x, card.y, card.width, card.height, card.radius);
        canvas->Stroke();
        canvas->SetFillStyle(kSky);
        canvas->BeginPath();
        canvas->MoveTo(184, 45);
        canvas->LineTo(201, 51);
        canvas->LineTo(184, 57);
        canvas->ClosePath();
        canvas->Fill();
    }
    canvas->Restore();

    const std::string& prompt = state.card.cue == TVideoCardCue::kRetry ? copy.retry : copy.prompt;
    std::vector<std::string> lines;
    int font_index = -1;
    for (int index = 0; index < 4 && font_index < 0; ++index) {
        canvas->SetFont(kPromptFonts[index]);
        if (WrapPromptLines(canvas, prompt, &lines)) font_index = index;
    }
    if (font_index < 0) return "prompt does not fit";
    canvas->Save();
    canvas->BeginPath();
    canvas->Rect(card.x, card.y, card.width, card.height);
    canvas->Clip();
    canvas->SetFillStyle(kInk);
    canvas->SetFont(kPromptFonts[font_index]);
    canvas->SetTextAlign("left");
    canvas->SetTextBaseline("alphabetic");
    const double line_height = kPromptFontSizes[font_index] + 5;
    const double first_baseline =
        card.y + card.height / 2 - ((static_cast<double>(lines.size()) - 1) * line_height) / 2;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        canvas->FillText(lines[index], 42, first_baseline + static_cast<double>(index) * line_height);
    }
    canvas->Restore();
    canvas->Restore();
    return nullptr;
}

void PaintCorrectChip(TVideoCanvas* canvas, const std::string& label, const TVideoFrameLayout& layout) {
    const auto& chip = layout.correct_chip;
    canvas->Save();
    canvas->SetGlobalAlpha(chip.opacity);
    canvas->Translate(chip.x, chip.y);
    canvas->Scale(chip.scale, chip.scale);
    canvas->SetFont("700 16px \"Noto Sans\"");
    canvas->SetTextAlign("left");
    canvas->SetTextBaseline("alphabetic");
    const double width = std::min(190.0, std::max(118.0, canvas->MeasureText(label) + 26));
    canvas->SetFillStyle("#3fbc9d");
    RoundedPath(canvas, 0, 0, width, chip.height, 14.5);
    canvas->Fill();
    canvas->SetFillStyle("#ffffff");
    canvas->FillText(label, 13, 21);
    canvas->Restore();
}

}  // namespace

const char* PaintTVideoFrame(TVideoCanvas* canvas, const TVideoFrameState& state, const TVideoFrameLayout& layout,
                             const TVideoCopy& copy) {
    canvas->SetFillStyle("#fff7df");
    canvas->FillRect(0, 0, 480, 320);
    canvas->DrawMedia(TVideoMedia::kBackground, 0, 0, 480, 320);

    canvas->Save();
    canvas->SetGlobalAlpha(layout.object.opacity);
    canvas->DrawMedia(TVideoMedia::kTeachingObject, layout.object.x, layout.object.y, layout.object.size,
                      layout.object.size);
    canvas->Restore();
    if (layout.word_pill.visible) PaintWordPill(canvas, copy.label, layout);

    if (layout.has_shadow) {
        canvas->Save();
        canvas->SetGlobalAlpha(layout.shadow.opacity);
        canvas->SetFillStyle("rgba(28,44,54,.34)");
        canvas->BeginPath();
        canvas->Ellipse(layout.shadow.center_x, layout.shadow.center_y, layout.shadow.radius_x, layout.shadow.radius_y,
                        0, 0, kPi * 2);
        canvas->Fill();
        canvas->Restore();
        canvas->SetGlobalAlpha(1);
    }
    for (int index = 0; index < layout.puff_count; ++index) {
        const auto& particle = layout.puff[index];
        canvas->Save();
        canvas->SetGlobalAlpha(particle.opacity);
        canvas->SetFillStyle(kPuffColors[particle.color_index % 5]);
        canvas->BeginPath();
        canvas->Arc(particle.center_x, particle.center_y, particle.radius, 0, kPi * 2);
        canvas->Fill();
        canvas->Restore();
    }
    if (layout.puff_count > 0) canvas->SetGlobalAlpha(1);

    canvas->Save();
    canvas->SetGlobalAlpha(layout.robot.opacity);
    canvas->Translate(layout.robot.anchor_x, layout.robot.anchor_y);
    canvas->Scale(layout.robot.scale_x, layout.robot.scale_y);
    const double base = layout.robot.base_size;
    canvas->DrawMedia(RobotMedia(layout.robot.clip_role), -base / 2, -base, base, base);
    canvas->Restore();

    for (const auto& dot : layout.progress_dots) {
        canvas->Save();
        canvas->SetGlobalAlpha(1);
        canvas->SetFillStyle(dot.active ? kSun : "rgba(255,255,255,.72)");
        canvas->SetStrokeStyle(dot.active ? "#d8a800" : "rgba(14,34,48,.28)");
        canvas->SetLineWidth(1);
        canvas->BeginPath();
        canvas->Arc(dot.center_x, dot.center_y, dot.radius, 0, kPi * 2);
        canvas->Fill();
        canvas->Stroke();
        canvas->Restore();
    }
    if (!layout.progress_dots.empty()) canvas->SetGlobalAlpha(1);
    if (layout.has_card) {
        if (const char* error = PaintCard(canvas, state, layout, copy)) return error;
    }
    if (layout.has_correct_chip) PaintCorrectChip(canvas, copy.correct, layout);
    for (int index = 0; index < layout.confetti_count; ++index) {
        const auto& piece = layout.confetti[index];
        canvas->Save();
        canvas->SetGlobalAlpha(piece.opacity);
        canvas->SetFillStyle(kConfettiColors[piece.color_index % 6]);
        canvas->Translate(piece.center_x, piece.center_y);
        canvas->Rotate(piece.rotation_rad);
        canvas->FillRect(-piece.size / 2, -piece.size / 2, piece.size, piece.size);
        canvas->Restore();
    }
    return nullptr;
}

}  // namespace tbot
