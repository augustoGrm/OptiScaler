// Host check of DlssNr_ColourEncoding.h: which colour encoding the NR pass decodes the game's frame with. No GPU and
// no game needed.
// cl /std:c++20 /EHsc tests/nr_colour_encoding_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ColourEncoding.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace DlssNrColourEncoding;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    // Auto is exactly today's rule: linear HDR only when the game says HDR and the format can hold it.
    {
        const auto hdr = Resolve(kAuto, true, true);
        CHECK(hdr.encoding == Encoding::LinearHdr);
        CHECK(hdr.automatic && !hdr.mismatch);
        CHECK(hdr.LinearHdr());

        for (bool flag : { false, true })
            for (bool canHold : { false, true })
            {
                if (flag && canHold)
                    continue;
                const auto sdr = Resolve(kAuto, flag, canHold);
                CHECK(sdr.encoding == Encoding::Srgb);
                CHECK(sdr.automatic && !sdr.mismatch);
                CHECK(!sdr.LinearHdr());
            }
    }

    // The Auto reason names both inputs, so the menu can say why.
    {
        CHECK(std::strcmp(Resolve(kAuto, true, true).reason, "HDR flag on, float format") == 0);
        CHECK(std::strcmp(Resolve(kAuto, true, false).reason, "HDR flag on, but the format cannot hold linear HDR") == 0);
        CHECK(std::strcmp(Resolve(kAuto, false, true).reason, "HDR flag off") == 0);
        CHECK(std::strcmp(Resolve(kAuto, false, false).reason, "HDR flag off") == 0);
    }

    // A forced choice is used whatever the game says (Linear HDR only where the format can hold it; see below).
    {
        for (bool flag : { false, true })
            for (bool canHold : { false, true })
            {
                CHECK(Resolve(1, flag, canHold).encoding == (canHold ? Encoding::LinearHdr : Encoding::Srgb));
                CHECK(Resolve(2, flag, canHold).encoding == Encoding::Srgb);
                CHECK(Resolve(3, flag, canHold).encoding == Encoding::Gamma22);
                CHECK(Resolve(4, flag, canHold).encoding == Encoding::Pq);
                for (uint32_t forced = 1; forced <= 4; ++forced)
                    CHECK(!Resolve(forced, flag, canHold).automatic);
            }
        // PQ is decoded to linear light, so the linear-HDR path handles it after the decode.
        CHECK(Resolve(4, false, false).LinearHdr());
        CHECK(!Resolve(3, true, true).LinearHdr());
    }

    // Linear HDR forced on a format that cannot hold it is refused: the frame is read as Auto reads it (tone-mapped
    // sRGB, since the format already rules linear HDR out), and the choice says so. Reading gamma values as linear light
    // made the Automatic white point swing 0.15x-12x (Call to Arms).
    for (bool flag : { false, true })
    {
        const auto refused = Resolve(1, flag, false);
        CHECK(refused.refused && !refused.mismatch && !refused.automatic);
        CHECK(refused.encoding == Encoding::Srgb && !refused.LinearHdr());
        CHECK(!Resolve(1, flag, true).refused && Resolve(1, flag, true).encoding == Encoding::LinearHdr);
    }

    // PQ in a float format is unusual but possible, so it is flagged and still applied.
    {
        CHECK(Resolve(4, false, true).mismatch && !Resolve(4, false, true).refused);
        CHECK(Resolve(4, false, true).encoding == Encoding::Pq);
        CHECK(!Resolve(4, false, false).mismatch);
        // Tone-mapped values in a float buffer are ordinary (the FP16-with-gamma case), not a mismatch.
        CHECK(!Resolve(2, false, true).mismatch);
        CHECK(!Resolve(3, false, true).mismatch);
        // Nothing else is ever refused.
        for (uint32_t setting = 0; setting < kSettingCount; ++setting)
            for (bool canHold : { false, true })
                CHECK(Resolve(setting, true, canHold).refused == (setting == 1 && !canHold));
    }

    // An out-of-range ini value behaves as Auto rather than as some other choice.
    {
        CHECK(Resolve(99, true, true).automatic);
        CHECK(Resolve(99, true, true).encoding == Encoding::LinearHdr);
        CHECK(Resolve(99, false, true).encoding == Encoding::Srgb);
    }

    // Names for the menu and log, one per setting value.
    {
        CHECK(std::strcmp(Name(Encoding::LinearHdr), "Linear HDR") == 0);
        CHECK(std::strcmp(Name(Encoding::Srgb), "Tone-mapped sRGB") == 0);
        CHECK(std::strcmp(Name(Encoding::Gamma22), "Tone-mapped gamma 2.2") == 0);
        CHECK(std::strcmp(Name(Encoding::Pq), "PQ (HDR10)") == 0);
        CHECK(kSettingCount == 5);
    }

    // Finished Picture: Auto is today's screen colour-space rule, a forced choice picks the path and lifts the refusal.
    {
        // Auto, supported combinations exactly as before.
        CHECK(ResolveFinished(kAuto, true, Screen::Sdr, false, true).supported);
        CHECK(ResolveFinished(kAuto, true, Screen::Pq, false, true).supported);
        CHECK(ResolveFinished(kAuto, true, Screen::Scrgb, true, false).supported);
        // Auto, refused exactly as before: FP16 that is not scRGB, scRGB that is not FP16, unknown colour space.
        CHECK(!ResolveFinished(kAuto, true, Screen::Sdr, true, false).supported);
        CHECK(!ResolveFinished(kAuto, true, Screen::Scrgb, false, true).supported);
        CHECK(!ResolveFinished(kAuto, false, Screen::Sdr, false, true).supported);
        CHECK(ResolveFinished(kAuto, true, Screen::Pq, false, true).screen == Screen::Pq);
        CHECK(ResolveFinished(kAuto, true, Screen::Scrgb, true, false).shaderConversion == 0);
        CHECK(ResolveFinished(kAuto, true, Screen::Sdr, false, true).choice.automatic);
        CHECK(std::strcmp(ResolveFinished(kAuto, true, Screen::Pq, false, true).choice.reason,
                          "screen colour space HDR10") == 0);
        // An HDR10 picture is R10G10B10A2_UNORM (native input decodes it to scRGB itself, so the shared pass converts
        // nothing); an FP16 buffer reported as HDR10 stays refused.
        {
            const auto hdr10 = ResolveFinished(kAuto, true, Screen::Pq, false, true);
            CHECK(hdr10.supported && hdr10.screen == Screen::Pq && hdr10.shaderConversion == 0);
            CHECK(!ResolveFinished(kAuto, true, Screen::Pq, true, false).supported);
        }

        // Forced sRGB / gamma 2.2 on an FP16 buffer: the case Auto refuses, now taken as SDR.
        const auto srgbOnFp16 = ResolveFinished(2, true, Screen::Scrgb, true, false);
        CHECK(srgbOnFp16.supported && srgbOnFp16.screen == Screen::Sdr && srgbOnFp16.shaderConversion == 0);
        CHECK(!srgbOnFp16.choice.automatic && !srgbOnFp16.choice.mismatch);
        const auto gammaOnFp16 = ResolveFinished(3, false, Screen::Sdr, true, false);
        CHECK(gammaOnFp16.supported && gammaOnFp16.screen == Screen::Sdr && gammaOnFp16.shaderConversion == 3);
        // Forced PQ on Finished Picture is decoded by its own conversion pass: nothing left for the shared pass.
        CHECK(ResolveFinished(4, true, Screen::Sdr, false, true).shaderConversion == 0);

        // Forced linear HDR on a UNORM screen is refused (an 8/10-bit screen is not scRGB): the SDR path, as Auto.
        const auto linearOnUnorm = ResolveFinished(1, true, Screen::Sdr, false, true);
        CHECK(linearOnUnorm.supported && linearOnUnorm.screen == Screen::Sdr && linearOnUnorm.choice.refused);
        CHECK(linearOnUnorm.shaderConversion == 0);
        const auto linearOnFp16 = ResolveFinished(1, true, Screen::Sdr, true, false);
        CHECK(linearOnFp16.supported && linearOnFp16.screen == Screen::Scrgb && !linearOnFp16.choice.refused);
        // Forced PQ on an FP16 screen is applied with a warning.
        const auto pqOnFp16 = ResolveFinished(4, true, Screen::Scrgb, true, false);
        CHECK(pqOnFp16.supported && pqOnFp16.screen == Screen::Pq && pqOnFp16.choice.mismatch);
        CHECK(!ResolveFinished(4, true, Screen::Sdr, false, true).choice.mismatch);

        // A format none of the paths can handle stays refused even when forced.
        CHECK(!ResolveFinished(2, true, Screen::Sdr, false, false).supported);
    }

    // What the shared pass converts: gamma 2.2 and PQ; Auto's outcomes and linear HDR / sRGB convert nothing.
    {
        CHECK(ShaderConversion(Encoding::LinearHdr) == 0);
        CHECK(ShaderConversion(Encoding::Srgb) == 0);
        CHECK(ShaderConversion(Encoding::Gamma22) == 3);
        CHECK(ShaderConversion(Encoding::Pq) == 4);
        for (bool flag : { false, true })
            for (bool canHold : { false, true })
                CHECK(ShaderConversion(Resolve(kAuto, flag, canHold).encoding) == 0);
        CHECK(!ShaderConverts(0) && !ShaderConverts(1) && !ShaderConverts(2));
        CHECK(ShaderConverts(3) && ShaderConverts(4));
        CHECK(!ShaderConverts(99));
    }

    // The menu's status line and the mismatch warning.
    {
        CHECK(Describe(Resolve(kAuto, true, true), "R16G16B16A16_FLOAT") ==
              "Auto: Linear HDR (HDR flag on, float format; R16G16B16A16_FLOAT)");
        CHECK(Describe(Resolve(4, true, false), "R10G10B10A2_UNORM") == "Forced: PQ (HDR10); R10G10B10A2_UNORM");
        CHECK(Describe(Resolve(kAuto, false, true), nullptr) == "Auto: Tone-mapped sRGB (HDR flag off; )");
        CHECK(std::strlen(MismatchWarning(Resolve(4, false, false))) == 0);
        CHECK(std::strlen(MismatchWarning(Resolve(4, false, true))) > 0);
        CHECK(std::strlen(MismatchWarning(Resolve(1, false, false))) > 0);
        CHECK(std::strlen(MismatchWarning(Resolve(1, false, true))) == 0);
        CHECK(Describe(Resolve(1, true, false), "B8G8R8A8_UNORM") ==
              "Forced: Linear HDR refused (the format cannot hold it), read as Tone-mapped sRGB; B8G8R8A8_UNORM");
        CHECK(std::strlen(MismatchWarning(Resolve(kAuto, true, false))) == 0);
    }

    if (fails)
    {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("nr_colour_encoding_smoke: all checks passed\n");
    return 0;
}
