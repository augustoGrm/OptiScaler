#pragma once

// Which colour encoding the NR pass decodes the game's frame with.
//
// Auto is the rule this pass has always used: the frame is linear HDR only when the game's DLSS IsHDR flag is set AND
// the format can hold linear light (FormatCanHoldLinearHdr); anything else is treated as a tone-mapped sRGB picture.
// The flag is a statement of intent a game can get wrong in either direction, and "tone-mapped" is not always sRGB,
// so [DlssNr] ColourEncoding lets the user name the encoding instead. A forced choice is applied even when the format
// looks unusual for it, and the mismatch is reported -- with one exception: Linear HDR on a format that cannot hold
// linear light (8/10-bit UNORM) is refused and the frame read as Auto reads it. Such a frame holds gamma-encoded values
// whatever the user believes; reading them as linear light darkens and flattens what the model is shown and made the
// Automatic white point swing 0.15x-12x frame to frame (Call to Arms, 8-bit B8G8R8A8).
//
// PQ is decoded to linear light (1.0 = 203 nits, the ITU-R BT.2408 reference white) and then goes through the
// linear-HDR path, so LinearHdr() is true for it.
//
// Header-only and free of D3D and Vulkan types so it can be exercised on the host (tests/nr_colour_encoding_smoke.cpp).
// The numeric values are the ini values and the shader's InputEncoding; keep all three in step.

#include <cstdint>
#include <cstdio>
#include <string>

namespace DlssNrColourEncoding
{
enum class Encoding : uint32_t
{
    LinearHdr = 1,
    Srgb = 2,
    Gamma22 = 3,
    Pq = 4,
};

constexpr uint32_t kAuto = 0;
constexpr uint32_t kSettingCount = 5; // Auto + the four encodings, the menu's combo length

struct Choice
{
    Encoding encoding = Encoding::Srgb;
    bool automatic = true;
    bool mismatch = false;   // forced, and the format looks wrong for it (applied anyway)
    bool refused = false;    // forced Linear HDR on a format that cannot hold it: read as Auto reads it instead
    const char* reason = ""; // why Auto chose what it chose; empty when forced

    // Whether the frame reaches the model through the linear-HDR path (white point, proxy curve).
    bool LinearHdr() const { return encoding == Encoding::LinearHdr || encoding == Encoding::Pq; }
};

inline const char* Name(Encoding encoding)
{
    switch (encoding)
    {
    case Encoding::LinearHdr:
        return "Linear HDR";
    case Encoding::Srgb:
        return "Tone-mapped sRGB";
    case Encoding::Gamma22:
        return "Tone-mapped gamma 2.2";
    case Encoding::Pq:
        return "PQ (HDR10)";
    }
    return "Unknown";
}

// setting: [DlssNr] ColourEncoding. gameSaysHdr: the game's DLSS IsHDR create flag. formatCanHoldHdr: the
// backend's FormatCanHoldLinearHdr for the frame's colour-authority format.
inline Choice Resolve(uint32_t setting, bool gameSaysHdr, bool formatCanHoldHdr)
{
    Choice choice;

    if (setting == kAuto || setting >= kSettingCount)
    {
        choice.encoding = gameSaysHdr && formatCanHoldHdr ? Encoding::LinearHdr : Encoding::Srgb;
        choice.reason = !gameSaysHdr       ? "HDR flag off"
                        : formatCanHoldHdr ? "HDR flag on, float format"
                                           : "HDR flag on, but the format cannot hold linear HDR";
        return choice;
    }

    choice.encoding = static_cast<Encoding>(setting);
    choice.automatic = false;

    // Auto's own outcome on such a format: it cannot be linear HDR, so tone-mapped sRGB.
    if (choice.encoding == Encoding::LinearHdr && !formatCanHoldHdr)
    {
        choice.encoding = Encoding::Srgb;
        choice.refused = true;
        return choice;
    }

    choice.mismatch = choice.encoding == Encoding::Pq && formatCanHoldHdr;
    return choice;
}

// What the shared pass converts at its edges (DlssNrConstants::InputEncoding): gamma 2.2 and PQ; linear HDR and
// sRGB are already the pass's own domains, so they convert nothing. Kept apart from the Encoding itself, which is
// what the log and the menu name: Finished Picture decodes PQ with its own pass and converts nothing here.
inline uint32_t ShaderConversion(Encoding encoding)
{
    return encoding == Encoding::Gamma22 || encoding == Encoding::Pq ? (uint32_t) encoding : 0u;
}

inline bool ShaderConverts(uint32_t shaderConversion)
{
    return shaderConversion == (uint32_t) Encoding::Gamma22 || shaderConversion == (uint32_t) Encoding::Pq;
}

// Finished Picture reads the swapchain, not a DLSS evaluate, so Auto there is the screen's colour space rather than
// the DLSS flag. The three screen paths it has: SDR (tone-mapped, passed through), scRGB (linear light, 1.0 = 80 nits)
// and PQ (decoded to linear before the pass). A forced choice picks the path whatever the colour space says, and is
// allowed on any of the formats those paths handle (8/10-bit UNORM, FP16) -- that is what lets an FP16 buffer holding
// gamma values through as SDR instead of being refused. Linear HDR forced on a UNORM screen is refused as Resolve
// refuses it (an 8/10-bit screen is not scRGB) and takes the SDR path.
enum class Screen : uint32_t
{
    Sdr,
    Scrgb,
    Pq,
};

struct FinishedChoice
{
    bool supported = false;
    Screen screen = Screen::Sdr;
    uint32_t shaderConversion = 0; // for the shared pass: 3 when gamma 2.2 is re-encoded as sRGB, else 0
    Choice choice;
};

// detectedKnown: the colour space is one of the three. detected: which. isFp16 / isUnorm: R16G16B16A16_FLOAT, or
// R8G8B8A8_UNORM / R10G10B10A2_UNORM.
inline FinishedChoice ResolveFinished(uint32_t setting, bool detectedKnown, Screen detected, bool isFp16, bool isUnorm)
{
    FinishedChoice out;

    if (setting == kAuto || setting >= kSettingCount)
    {
        out.screen = detected;
        out.supported = detectedKnown && (detected == Screen::Scrgb ? isFp16 : isUnorm);
        out.choice.encoding = detected == Screen::Pq      ? Encoding::Pq
                              : detected == Screen::Scrgb ? Encoding::LinearHdr
                                                          : Encoding::Srgb;
        out.choice.reason = !detectedKnown              ? "screen colour space not supported"
                            : detected == Screen::Pq    ? "screen colour space HDR10"
                            : detected == Screen::Scrgb ? "screen colour space scRGB"
                                                        : "screen colour space SDR";
        return out;
    }

    out.choice = Resolve(setting, false, isFp16);
    out.supported = isFp16 || isUnorm;
    out.screen = out.choice.encoding == Encoding::LinearHdr ? Screen::Scrgb
                 : out.choice.encoding == Encoding::Pq      ? Screen::Pq
                                                            : Screen::Sdr;
    // PQ is decoded by Finished Picture's own conversion pass, so only gamma 2.2 is left to the shared pass.
    out.shaderConversion = out.choice.encoding == Encoding::Gamma22 ? ShaderConversion(Encoding::Gamma22) : 0u;
    return out;
}

// Shown when a forced choice does not fit the buffer. Empty when it fits.
inline const char* MismatchWarning(const Choice& choice)
{
    if (choice.refused)
        return "This buffer is not a float format, so it cannot hold linear HDR. Read as Tone-mapped sRGB instead.";
    if (!choice.mismatch)
        return "";
    return "This buffer is a float format, which PQ output rarely is. Applied anyway.";
}

// The menu's line: "Auto: Linear HDR (HDR flag on, float format; R16G16B16A16_FLOAT)" or "Forced: PQ (HDR10);
// R10G10B10A2_UNORM". The store that holds the last frame's choice is dlssnr/DlssNr_ColourEncodingStatus.cpp.
inline std::string Describe(const Choice& choice, const char* formatName)
{
    char text[192];
    if (choice.automatic)
        std::snprintf(text, sizeof(text), "Auto: %s (%s; %s)", Name(choice.encoding), choice.reason,
                      formatName ? formatName : "");
    else if (choice.refused)
        std::snprintf(text, sizeof(text), "Forced: Linear HDR refused (the format cannot hold it), read as %s; %s",
                      Name(choice.encoding), formatName ? formatName : "");
    else
        std::snprintf(text, sizeof(text), "Forced: %s; %s", Name(choice.encoding), formatName ? formatName : "");
    return text;
}
} // namespace DlssNrColourEncoding
