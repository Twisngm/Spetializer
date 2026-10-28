#pragma once

namespace Spatializer
{
    // 기존 씬과 SpatializerGain이 0~3번을 사용하므로 기존 번호를 유지하고 새 파라미터는 끝에만 추가한다.
    enum Parameter
    {
        P_AUDIOSRCATTN, P_FIXEDVOLUME, P_CUSTOMFALLOFF, P_GAIN,
        P_MULTIBAND, P_CROSSOVER_LOW, P_CROSSOVER_MID, P_CROSSOVER_HIGH,
        P_BAND_GAIN_LOW, P_BAND_GAIN_LOW_MID, P_BAND_GAIN_HIGH_MID, P_BAND_GAIN_HIGH,
        P_BAND_DRIVE_LOW, P_BAND_DRIVE_LOW_MID, P_BAND_DRIVE_HIGH_MID, P_BAND_DRIVE_HIGH,
        P_BAND_WET_LOW, P_BAND_WET_LOW_MID, P_BAND_WET_HIGH_MID, P_BAND_WET_HIGH,
        P_MOTION_SMOOTHING_MS,
        P_NUM
    };

    struct ParameterDefinition
    {
        const char* name;
        const char* unit;
        float minimum, maximum, initial;
        const char* description;
    };

    static const ParameterDefinition Parameters[P_NUM] =
    {
        { "AudioSrc Attn", "", 0, 1, 1, "AudioSource distance attenuation" },
        { "Fixed Volume", "", 0, 1, 0, "Fixed volume amount" },
        { "Custom Falloff", "", 0, 1, 0, "Custom volume falloff amount (logarithmic)" },
        { "Gain", "x", 0, 4, 2, "Linear output gain (0 = mute, 1 = unity gain)" },
        { "4 Band Enable", "", 0, 1, 0, "Enable the four-band processor before spatial panning" },
        { "Low Split", "Hz", 20, 20000, 250, "Low / low-mid crossover (static configuration)" },
        { "Mid Split", "Hz", 20, 20000, 2000, "Low-mid / high-mid crossover (static configuration)" },
        { "High Split", "Hz", 20, 20000, 6000, "High-mid / high crossover (static configuration)" },
        { "Low Gain", "x", 0, 4, 1, "Low band linear gain" },
        { "Low Mid Gain", "x", 0, 4, 1, "Low-mid band linear gain" },
        { "High Mid Gain", "x", 0, 4, 1, "High-mid band linear gain" },
        { "High Gain", "x", 0, 4, 1, "High band linear gain" },
        { "Low Drive", "x", 1, 8, 1, "Low band example saturation drive" },
        { "Low Mid Drive", "x", 1, 8, 1, "Low-mid band example saturation drive" },
        { "High Mid Drive", "x", 1, 8, 1, "High-mid band example saturation drive" },
        { "High Drive", "x", 1, 8, 1, "High band example saturation drive" },
        { "Low Wet", "", 0, 1, 0, "Low band saturation wet mix (0 = bypass)" },
        { "Low Mid Wet", "", 0, 1, 0, "Low-mid band saturation wet mix (0 = bypass)" },
        { "High Mid Wet", "", 0, 1, 0, "High-mid band saturation wet mix (0 = bypass)" },
        { "High Wet", "", 0, 1, 0, "High band saturation wet mix (0 = bypass)" },
        { "Motion Smooth", "ms", 0, 100, 5, "Sample-wise pan, blend and gain ramp time (0 = immediate)" }
    };
}
