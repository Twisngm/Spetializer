// Please note that this will only work on Unity 5.2 or higher.

#include "AudioPluginUtil.h"
#include "SpatializerDSP.h"
#include "SpatializerParameters.h"
#include <atomic>

namespace Spatializer
{
    // Single shared snapshot: listener-local x/y/z, azimuth/elevation in degrees.
    // Retains the last published values until the native library is unloaded.
    static float currentSpatialization[5] = { 0 };
    static bool hasCurrentSpatialization = false;
    static std::atomic_flag spatializationLock = ATOMIC_FLAG_INIT;

    static void StoreCurrentSpatialization(float x, float y, float z, float azimuth, float elevation)
    {
        // Never wait on the audio thread. Skip this update if a reader is copying.
        if (spatializationLock.test_and_set(std::memory_order_acquire))
            return;

        currentSpatialization[0] = x;
        currentSpatialization[1] = y;
        currentSpatialization[2] = z;
        currentSpatialization[3] = azimuth;
        currentSpatialization[4] = elevation;
        hasCurrentSpatialization = true;
        spatializationLock.clear(std::memory_order_release);
    }

    struct EffectData
    {
        std::atomic<float> p[P_NUM];
        SpatializerDSP::FourBandProcessor multiband;
        SpatializerDSP::MotionInterpolator motion;
        UInt64 nextDspTick;
        bool hasDspTick;

        EffectData() : nextDspTick(0), hasDspTick(false) {}
        void Reset()
        {
            multiband.Reset();
            motion.Reset();
            hasDspTick = false;
        }
    };

    // 다른 EffectModel에서도 사용하기 위해 연산된 값을 지역 변수에서 구조체 형태로 설계 변경
    struct SpatialGeometry
    {
        float x, y, z;
        float horizontalDistance;
        float distance; // 추후 감쇠모델 적용 시 사용 예정. 현재는 연산만 진행하고 적용 x
        float azimuthDegrees;
        float elevationDegrees;
    };

    struct StereoGains // Equal-Power Panning을 위한 좌우 계수
    {
        float left;
        float right;
    };

    static SpatialGeometry GeometryCalc(const UnityAudioSpatializerData& spatializer)
    {
        const float* m = spatializer.listenermatrix;
        const float* s = spatializer.sourcematrix;
        const float px = s[12], py = s[13], pz = s[14];
        const float kRad2Deg = 180.0f / AudioPluginUtil::kPI;

        SpatialGeometry geometry;
        geometry.x = m[0] * px + m[4] * py + m[8] * pz + m[12];
        geometry.y = m[1] * px + m[5] * py + m[9] * pz + m[13];
        geometry.z = m[2] * px + m[6] * py + m[10] * pz + m[14];
        geometry.horizontalDistance = sqrtf(geometry.x * geometry.x + geometry.z * geometry.z);
        geometry.distance = sqrtf(geometry.x * geometry.x + geometry.y * geometry.y + geometry.z * geometry.z);

        float azimuth = 0.0f;
        if (geometry.horizontalDistance > 1.0e-6f)
        {
            azimuth = atan2f(geometry.x, geometry.z);
            if (azimuth < 0.0f)
                azimuth += 2.0f * AudioPluginUtil::kPI;
        }
        geometry.azimuthDegrees = AudioPluginUtil::FastClip(azimuth * kRad2Deg, 0.0f, 360.0f);
        geometry.elevationDegrees = atan2f(geometry.y, geometry.horizontalDistance) * kRad2Deg;
        return geometry;
    }

    static float CalculatePan(const SpatialGeometry& geometry)
    {
        // 수평 거리가 없는 수직 상태의 음원의 경우 0으로 정규화
        if (geometry.horizontalDistance <= 1.0e-6f)
            return 0.0f;
        return AudioPluginUtil::FastClip(geometry.x / geometry.horizontalDistance, -1.0f, 1.0f);
    }

    static StereoGains CalculateEqualPowerGains(float pan)
    {
        // 계산식의 한계로 양 극단의 경우를 별도 조건문 처리
        if (pan <= -1.0f)
        {
            const StereoGains gains = { 1.0f, 0.0f };
            return gains;
        }
        if (pan >= 1.0f)
        {
            const StereoGains gains = { 0.0f, 1.0f };
            return gains;
        }

        const float angle = (pan + 1.0f) * AudioPluginUtil::kPI * 0.25f;
        const StereoGains gains = { cosf(angle), sinf(angle) };
        return gains;
    }

    inline bool IsHostCompatible(UnityAudioEffectState* state)
    {
        // Somewhat convoluted error checking here because hostapiversion is only supported from SDK version 1.03 (i.e. Unity 5.2) and onwards.
        // Since we are only checking for version 0x010300 here, we can't use newer fields in the UnityAudioSpatializerData struct, such as minDistance and maxDistance.
        return
            state->structsize >= sizeof(UnityAudioEffectState) &&
            state->hostapiversion >= 0x010300;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ResetCallback(UnityAudioEffectState* state);

    int InternalRegisterEffectDefinition(UnityAudioEffectDefinition& definition)
    {
        int numparams = P_NUM;
        definition.paramdefs = new UnityAudioParameterDefinition[numparams];
        for (int i = 0; i < P_NUM; ++i)
        {
            const ParameterDefinition& p = Parameters[i];
            AudioPluginUtil::RegisterParameter(definition, p.name, p.unit, p.minimum, p.maximum, p.initial, 1, 1, i, p.description);
        }
        definition.flags |= UnityAudioEffectDefinitionFlags_IsSpatializer;
        definition.reset = ResetCallback;
        return numparams;
    }

    static UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK DistanceAttenuationCallback(UnityAudioEffectState* state, float distanceIn, float attenuationIn, float* attenuationOut)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        *attenuationOut =
            data->p[P_AUDIOSRCATTN].load(std::memory_order_relaxed) * attenuationIn +
            data->p[P_FIXEDVOLUME].load(std::memory_order_relaxed) +
            data->p[P_CUSTOMFALLOFF].load(std::memory_order_relaxed) * (1.0f / AudioPluginUtil::FastMax(1.0f, distanceIn));
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK CreateCallback(UnityAudioEffectState* state)
    {
        EffectData* effectdata = new EffectData;
        state->effectdata = effectdata;
        if (IsHostCompatible(state) && state->spatializerdata != NULL)
            state->spatializerdata->distanceattenuationcallback = DistanceAttenuationCallback;
        for (int i = 0; i < P_NUM; ++i)
            effectdata->p[i].store(Parameters[i].initial, std::memory_order_relaxed);
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ResetCallback(UnityAudioEffectState* state)
    {
        state->GetEffectData<EffectData>()->Reset();
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        delete data;
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(UnityAudioEffectState* state, int index, float value)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        if (index < 0 || index >= P_NUM)
            return UNITY_AUDIODSP_ERR_UNSUPPORTED;
        if (!SpatializerDSP::IsFinite(value))
            return UNITY_AUDIODSP_ERR_UNSUPPORTED;
        value = SpatializerDSP::Clamp(value, Parameters[index].minimum, Parameters[index].maximum);
        data->p[index].store(value, std::memory_order_relaxed);
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(UnityAudioEffectState* state, int index, float* value, char *valuestr)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        if (index < 0 || index >= P_NUM)
            return UNITY_AUDIODSP_ERR_UNSUPPORTED;
        if (value != NULL)
            *value = data->p[index].load(std::memory_order_relaxed);
        if (valuestr != NULL)
            valuestr[0] = 0;
        return UNITY_AUDIODSP_OK;
    }

    int UNITY_AUDIODSP_CALLBACK GetFloatBufferCallback(UnityAudioEffectState* state, const char* name, float* buffer, int numsamples)
    {
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ProcessCallback(UnityAudioEffectState* state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int outchannels)
    {
        if (length == 0) return UNITY_AUDIODSP_OK;
        // Check that I/O formats are right and that the host API supports this feature
        if (inchannels != 2 || outchannels != 2 ||
            !IsHostCompatible(state) || state->spatializerdata == NULL)
        {
            state->GetEffectData<EffectData>()->Reset();
            for (unsigned int n = 0; n < length; ++n)
                for (int c = 0; c < outchannels; ++c)
                    outbuffer[n * outchannels + c] = c < inchannels ? inbuffer[n * inchannels + c] : 0.0f; // in/out 채널 구성이 다를 경우 무음 처리
            return UNITY_AUDIODSP_OK;
        }

        EffectData* data = state->GetEffectData<EffectData>();
        // 음원이 가상화되거나 멈춘 뒤 재개될 때 오래된 위치 / 필터 잔향 재사용을 막는 부분
        if (data->hasDspTick && state->currdsptick != data->nextDspTick)
            data->Reset();
        data->nextDspTick = state->currdsptick + length;
        data->hasDspTick = true;

        float parameters[P_NUM];
        for (int i = 0; i < P_NUM; ++i)
            parameters[i] = data->p[i].load(std::memory_order_relaxed);
        SpatializerDSP::BandSettings bands[SpatializerDSP::BandCount];
        for (int b = 0; b < SpatializerDSP::BandCount; ++b)
            bands[b] = { parameters[P_BAND_GAIN_LOW + b], parameters[P_BAND_DRIVE_LOW + b], parameters[P_BAND_WET_LOW + b] };
        const float sampleRate = state->samplerate >= 1000 ? static_cast<float>(state->samplerate) : 48000.0f;
        data->multiband.Configure(sampleRate, &parameters[P_CROSSOVER_LOW], parameters[P_MULTIBAND] >= 0.5f, bands);
        const SpatialGeometry geometry = GeometryCalc(*state->spatializerdata);
        if (!SpatializerDSP::IsFinite(geometry.x) || !SpatializerDSP::IsFinite(geometry.y) ||
            !SpatializerDSP::IsFinite(geometry.z) || !SpatializerDSP::IsFinite(state->spatializerdata->spatialblend))
        {
            data->Reset();
            if (inbuffer != outbuffer) memcpy(outbuffer, inbuffer, sizeof(float) * length * 2);
            return UNITY_AUDIODSP_OK;
        }
        StoreCurrentSpatialization(geometry.x, geometry.y, geometry.z, geometry.azimuthDegrees, geometry.elevationDegrees);

        const SpatializerDSP::Position position = { geometry.x, geometry.y, geometry.z };
        const SpatializerDSP::SpatialControls target = {
            CalculatePan(geometry),
            SpatializerDSP::Clamp(state->spatializerdata->spatialblend, 0.0f, 1.0f),
            parameters[P_GAIN]
        };
        data->motion.Configure(position, target, sampleRate, parameters[P_MOTION_SMOOTHING_MS]);

        float previousPan = 2.0f; // 첫 샘플에서 반드시 좌우 계수를 계산하도록 유효 범위 밖의 값으로 시작
        StereoGains panGains = { 0, 0 };
        // 자체 모델 개발 이전까지 Distance 추가 감쇠 x
        for (unsigned int n = 0; n < length; ++n)
        {
            const SpatializerDSP::StereoFrame input = { inbuffer[n * 2], inbuffer[n * 2 + 1] };
            const SpatializerDSP::StereoFrame processed = data->multiband.Process(input);
            const float left = processed.left;
            const float right = processed.right;
            const SpatializerDSP::SpatialControls controls = data->motion.Next();
            if (controls.pan != previousPan)
            {
                panGains = CalculateEqualPowerGains(controls.pan);
                previousPan = controls.pan;
            }
            const float dryMix = 1.0f - controls.blend;
            // 입력 음원을 하나의 점으로 처리
            const float mono = 0.5f * (left + right);
            const float pannedLeft = mono * panGains.left;
            const float pannedRight = mono * panGains.right;
            outbuffer[n * 2] = (left * dryMix + pannedLeft * controls.blend) * controls.gain;
            outbuffer[n * 2 + 1] = (right * dryMix + pannedRight * controls.blend) * controls.gain;
        }

        return UNITY_AUDIODSP_OK;
    }
}

// C ABI for C#/DllImport (CallingConvention.Cdecl).
// values must point to at least five floats; extra elements are left unchanged.
// Order: listener-local x, y, z, azimuth (degrees), elevation (degrees).
// Returns 1 on success, 0 before the first snapshot or while busy, -1 for invalid arguments.
// On 0/-1 the caller's buffer is unchanged. Multiple sources share the latest snapshot.
extern "C" UNITY_AUDIODSP_EXPORT_API int Spatializer_GetCurrentSpatialization(float* values, int count)
{
    if (values == NULL || count < 5)
        return -1;
    if (Spatializer::spatializationLock.test_and_set(std::memory_order_acquire))
        return 0;

    const int result = Spatializer::hasCurrentSpatialization ? 1 : 0;
    if (result == 1)
        memcpy(values, Spatializer::currentSpatialization, sizeof(Spatializer::currentSpatialization));

    Spatializer::spatializationLock.clear(std::memory_order_release);
    return result;
}
