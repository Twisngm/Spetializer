#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// Unity에 의존하지 않고 음원별로 상태를 소유하도록 하며, 처리 중에는 메모리를 할당하지 않기 위한 헤더.
namespace SpatializerDSP
{
    const int BandCount = 4;
    const double Pi = 3.14159265358979323846;

    inline bool IsFinite(float value)
    {
        // 플러그인의 빌드 옵션에서 수행할 검사
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7f800000u) != 0x7f800000u;
    }

    inline float Clamp(float value, float low, float high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    inline int MillisecondsToSamples(float rate, float milliseconds)
    {
        return static_cast<int>(static_cast<double>(rate) * milliseconds / 1000.0 + 0.5);
    }

    class LinearRamp
    {
    public:
        explicit LinearRamp(float initial = 0) : current(initial), target(initial), step(0), remaining(0) {}
        void Snap(float value) { current = target = value; step = 0; remaining = 0; }
        void SetTarget(float value, int samples)
        {
            if (samples <= 0) { Snap(value); return; }
            if (value == target) return; // 프레임이나 블록마다 같은 값이 전달되어도 보간 재시도 금지
            target = value;
            remaining = samples;
            step = (target - current) / samples;
        }
        float Next()
        {
            if (remaining > 0)
            {
                current += step;
                if (--remaining == 0) current = target;
            }
            return current;
        }
        float Current() const { return current; }
        bool IsMoving() const { return remaining != 0; }
    private:
        float current, target, step;
        int remaining;
    };

    struct StereoFrame { float left, right; };

    struct Position { float x, y, z; };
    struct SpatialControls { float pan, blend, gain; };

    class MotionInterpolator
    {
    public:
        MotionInterpolator() : previous{ 0, 0, 0 }, position{ 0, 0, 0 }, sampleRate(0), initialized(false) {}
        void Reset() { initialized = false; }
        void Configure(const Position& targetPosition, const SpatialControls& target, float rate, float milliseconds)
        {
            const int samples = MillisecondsToSamples(rate, milliseconds);
            previous = initialized ? position : targetPosition;
            position = targetPosition;
            if (!initialized || sampleRate != rate)
            {
                pan.Snap(target.pan); blend.Snap(target.blend); gain.Snap(target.gain);
            }
            else
            {
                // 이전 보간이 진행 중이어도 새로운 값이 들어오면 마지막 출력값-새로운 목표로 보간 대상을 변경
                pan.SetTarget(target.pan, samples);
                blend.SetTarget(target.blend, samples);
                gain.SetTarget(target.gain, samples);
            }
            initialized = true;
            sampleRate = rate;
        }
        SpatialControls Next() { return { pan.Next(), blend.Next(), gain.Next() }; }
        Position PreviousPosition() const { return previous; }
    private:
        Position previous, position;
        LinearRamp pan, blend, gain;
        float sampleRate;
        bool initialized;
    };

    struct Coefficients { double b0, b1, b2, a1, a2; };

    class Biquad
    {
    public:
        Biquad() : z1(0), z2(0) {}
        void Reset() { z1 = z2 = 0; }
        double Process(double input, const Coefficients& c)
        {
            const double output = c.b0 * input + z1;
            z1 = c.b1 * input - c.a1 * output + z2;
            z2 = c.b2 * input - c.a2 * output;
            if (std::fabs(z1) < 1.0e-30) z1 = 0;
            if (std::fabs(z2) < 1.0e-30) z2 = 0;
            return output;
        }
    private:
        double z1, z2;
    };

    struct CrossoverDesign
    {
        Coefficients low, high, allpass;
        void Configure(float frequency, float sampleRate)
        {
            const double omega = 2 * Pi * frequency / sampleRate;
            const double cosine = std::cos(omega);
            const double alpha = std::sin(omega) / std::sqrt(2.0); // 진폭 완화 필터의 Q값은 1/sqrt(2)
            const double norm = 1 / (1 + alpha);
            const double a1 = -2 * cosine * norm;
            const double a2 = (1 - alpha) * norm;
            low = { (1 - cosine) * 0.5 * norm, (1 - cosine) * norm, (1 - cosine) * 0.5 * norm, a1, a2 };
            high = { (1 + cosine) * 0.5 * norm, -(1 + cosine) * norm, (1 + cosine) * 0.5 * norm, a1, a2 };
            // LR4 저역·고역 출력을 합친 전달함수가 해당 필터와 동일함.
            allpass = { a2, a1, 1, a1, a2 };
        }
    };

    class LR4Split
    {
    public:
        void Reset() { for (int i = 0; i < 2; ++i) { low[i].Reset(); high[i].Reset(); } }
        void Process(double input, const CrossoverDesign& c, double& lowOut, double& highOut)
        {
            lowOut = low[1].Process(low[0].Process(input, c.low), c.low);
            highOut = high[1].Process(high[0].Process(input, c.high), c.high);
        }
    private:
        Biquad low[2], high[2];
    };

    inline void SanitizeCrossovers(float sampleRate, const float* requested, float* result)
    {
        const float maximum = Clamp(sampleRate * 0.45f, 80, 20000);
        // Low/Mid/High의 순서를 유지하고 경계 주파수 사이 간격은 최소 1Hz로 제한
        result[0] = Clamp(IsFinite(requested[0]) ? requested[0] : 250, 20, maximum - 2);
        result[1] = Clamp(IsFinite(requested[1]) ? requested[1] : 2000, result[0] + 1, maximum - 1);
        result[2] = Clamp(IsFinite(requested[2]) ? requested[2] : 6000, result[1] + 1, maximum);
    }

    class FourBandSplitter
    {
    public:
        FourBandSplitter() : sampleRate(0), crossovers{ 0, 0, 0 } {}
        bool Configure(float rate, const float* frequencies)
        {
            rate = IsFinite(rate) && rate >= 1000 ? rate : 48000;
            float sanitized[3];
            SanitizeCrossovers(rate, frequencies, sanitized);
            bool changed = rate != sampleRate;
            for (int i = 0; i < 3; ++i) changed = changed || sanitized[i] != crossovers[i];
            if (!changed) return false;
            const bool newRate = rate != sampleRate;
            sampleRate = rate;
            for (int i = 0; i < 3; ++i)
            {
                crossovers[i] = sanitized[i];
                designs[i].Configure(crossovers[i], sampleRate);
            }
            if (newRate) Reset();
            return true;
        }
        void Reset()
        {
            for (int c = 0; c < 2; ++c)
            {
                channels[c].middle.Reset(); channels[c].lower.Reset(); channels[c].upper.Reset();
                channels[c].lowCompensation.Reset(); channels[c].highCompensation.Reset();
            }
        }
        void Process(const StereoFrame& input, StereoFrame* bands)
        {
            for (int c = 0; c < 2; ++c)
            {
                Channel& channel = channels[c];
                double lower, upper, b0, b1, b2, b3;
                channel.middle.Process(c == 0 ? input.left : input.right, designs[1], lower, upper);
                // 반대쪽 분기의 올패스를 적용해 4개 대역 출력의 위상을 조정
                lower = channel.lowCompensation.Process(lower, designs[2].allpass);
                upper = channel.highCompensation.Process(upper, designs[0].allpass);
                channel.lower.Process(lower, designs[0], b0, b1);
                channel.upper.Process(upper, designs[2], b2, b3);
                const double values[4] = { b0, b1, b2, b3 };
                for (int b = 0; b < BandCount; ++b)
                    (c == 0 ? bands[b].left : bands[b].right) = static_cast<float>(values[b]);
            }
        }
        const float* Frequencies() const { return crossovers; }
    private:
        struct Channel
        {
            LR4Split middle, lower, upper;
            Biquad lowCompensation, highCompensation;
        } channels[2];
        CrossoverDesign designs[3];
        float sampleRate, crossovers[3];
    };

    struct BandContext { int band; float sampleRate; };
    typedef void (*BandProcessCallback)(void* state, StereoFrame& frame, const BandContext& context);
    typedef void (*BandResetCallback)(void* state);

    // 슬롯은 해당 음원이 소유한 상태를 참조하며, 생성 시에만 등록하고 처리 중에는 변경 x
    // 콜백에서 메모리 할당이나 대기x 신호 지연을 추가x
    class BandEffectChain
    {
    public:
        static const int Capacity = 4;
        BandEffectChain() : count(0) {}
        bool Add(void* state, BandProcessCallback process, BandResetCallback reset = nullptr)
        {
            if (process == nullptr || count == Capacity) return false;
            slots[count++] = { state, process, reset };
            return true;
        }
        void Reset()
        {
            for (int i = 0; i < count; ++i) if (slots[i].reset) slots[i].reset(slots[i].state);
        }
        void Process(StereoFrame& frame, const BandContext& context)
        {
            for (int i = 0; i < count; ++i) slots[i].process(slots[i].state, frame, context);
        }
    private:
        struct Slot { void* state; BandProcessCallback process; BandResetCallback reset; };
        Slot slots[Capacity];
        int count;
    };

    struct BandSettings { float gain, drive, wet; };

    class BandProcessor
    {
    public:
        BandProcessor() : gain(1), drive(1), wet(0)
        {
            chain.Add(this, ProcessGain);
            chain.Add(this, ProcessSaturation);
        }
        BandProcessor(const BandProcessor&) = delete;
        BandProcessor& operator=(const BandProcessor&) = delete;
        void Configure(const BandSettings& settings, int rampSamples, bool snap)
        {
            const float g = Clamp(IsFinite(settings.gain) ? settings.gain : 1, 0, 4);
            const float d = Clamp(IsFinite(settings.drive) ? settings.drive : 1, 1, 8);
            const float w = Clamp(IsFinite(settings.wet) ? settings.wet : 0, 0, 1);
            if (snap) { gain.Snap(g); drive.Snap(d); wet.Snap(w); }
            else { gain.SetTarget(g, rampSamples); drive.SetTarget(d, rampSamples); wet.SetTarget(w, rampSamples); }
        }
        BandEffectChain chain;
    private:
        static void ProcessGain(void* state, StereoFrame& frame, const BandContext&)
        {
            const float value = static_cast<BandProcessor*>(state)->gain.Next();
            frame.left *= value; frame.right *= value;
        }
        static void ProcessSaturation(void* state, StereoFrame& frame, const BandContext&)
        {
            BandProcessor* band = static_cast<BandProcessor*>(state);
            const float d = band->drive.Next(), w = band->wet.Next();
            if (w == 0) return;
            // 기본값은 비활성화. 필요한 경우 조작을 통해 활성화해 사용하도록 구성
            const float norm = 1 / std::tanh(d);
            frame.left += w * (std::tanh(frame.left * d) * norm - frame.left);
            frame.right += w * (std::tanh(frame.right * d) * norm - frame.right);
        }
        LinearRamp gain, drive, wet;
    };

    class FourBandProcessor
    {
    public:
        FourBandProcessor() : sampleRate(0), initialized(false), processing(false) {}
        void Configure(float rate, const float* frequencies, bool enabled, const BandSettings* settings)
        {
            rate = IsFinite(rate) && rate >= 1000 ? rate : 48000;
            const bool snap = !initialized || sampleRate != rate;
            sampleRate = rate;
            splitter.Configure(rate, frequencies);
            const int rampSamples = MillisecondsToSamples(rate, 5); // 효과 파라미터를 5ms 동안 보간
            for (int b = 0; b < BandCount; ++b)
            {
                if (snap) bands[b].chain.Reset();
                bands[b].Configure(settings[b], rampSamples, snap);
            }
            if (snap) mix.Snap(enabled ? 1.0f : 0.0f);
            else mix.SetTarget(enabled ? 1.0f : 0.0f, rampSamples);
            initialized = true;
        }
        StereoFrame Process(const StereoFrame& input)
        {
            if (mix.Current() == 0 && !mix.IsMoving()) { processing = false; return input; }
            if (!processing) { splitter.Reset(); for (int b = 0; b < BandCount; ++b) bands[b].chain.Reset(); processing = true; }
            StereoFrame split[BandCount];
            splitter.Process(input, split);
            StereoFrame output = { 0, 0 };
            for (int b = 0; b < BandCount; ++b)
            {
                const BandContext context = { b, sampleRate };
                bands[b].chain.Process(split[b], context);
                output.left += split[b].left; output.right += split[b].right;
            }
            const float amount = mix.Next();
            if (amount == 1) return output;
            return { input.left + amount * (output.left - input.left), input.right + amount * (output.right - input.right) };
        }
        void Reset()
        {
            splitter.Reset();
            for (int b = 0; b < BandCount; ++b) bands[b].chain.Reset();
            initialized = processing = false;
        }
        BandProcessor bands[BandCount];
    private:
        FourBandSplitter splitter;
        LinearRamp mix;
        float sampleRate;
        bool initialized, processing;
    };
}
