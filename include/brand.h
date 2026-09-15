// Every string the brand owns, in one place. HARA LIGHT INDUSTRIES is a fake
// 1988 hardware company — a play on the name — so the voice is deliberately
// period: a BIOS vendor that also made cassette decks and neon.
//
// Keep the BIOS lines to 25 characters. That is the width of the boot
// console grid at u8g2_font_5x8_tf (128px / 5px cells) — widened
// 2026-09-16 from the original 32-char/4x6-cell grid to match mqttchan's
// message-screen font (see ui.h); anything longer is silently clipped
// mid-word and looks like a bug rather than a brand. (All the lines below
// already fit comfortably under the new limit — none needed rewording.)
#pragma once

namespace brand {

constexpr char NAME[]  = "HARA LIGHT INDUSTRIES";
constexpr char SHORT[] = "HLI";

// POST header block — what the video BIOS puts up before the self test.
constexpr char BIOS[] = "HLI/286 BIOS      v2.14";
constexpr char COPY[] = "(C) 1984 HARA LIGHT IND.";
constexpr char DOS[]  = "Starting HLI-DOS 4.20";

// Strap lines the splash cycles through once its intro has landed. All three
// must fit 128px at 4x6 with 1px tracking, i.e. 25 characters or fewer.
constexpr const char* KICKERS[] = {
    "HARA LIGHT INDUSTRIES",
    "PHOTONIC SYSTEMS DIV.",
    "EST. 1984 DUBLIN IE",
};
constexpr int KICKER_N = sizeof(KICKERS) / sizeof(KICKERS[0]);

}  // namespace brand
