using System;
using UnityEngine;

[DisallowMultipleComponent]
[RequireComponent(typeof(AudioSource))]
public sealed class SpatializerBands : MonoBehaviour
{
    public const float DefaultGain = 1f;
    public const float DefaultSaturationDrive = 1f;
    public const float DefaultSaturationWet = 0f;
    public const float DefaultLowCrossover = 250f;
    public const float DefaultMidCrossover = 2000f;
    public const float DefaultHighCrossover = 6000f;

    // 보조용 sealed class
    // SpatializerGain이 전체 출력 Gain으로 3번을 사용하니 변경 금지
    private const int EnableIndex = 4;
    private const int LowCrossoverIndex = 5;
    private const int BandGainIndex = 8;
    private const int BandDriveIndex = 12;
    private const int BandWetIndex = 16;
    private const int MotionSmoothingIndex = 20;

    [Serializable]
    private sealed class BandSettings
    {
        [Range(0f, 4f)] public float gain = DefaultGain;
        [Range(1f, 8f)] public float saturationDrive = DefaultSaturationDrive;
        [Range(0f, 1f)] public float saturationWet = DefaultSaturationWet;

        public void Validate()
        {
            gain = FiniteClamp(gain, 0f, 4f, DefaultGain);
            saturationDrive = FiniteClamp(saturationDrive, 1f, 8f, DefaultSaturationDrive);
            saturationWet = FiniteClamp(saturationWet, 0f, 1f, DefaultSaturationWet);
        }
    }

    [SerializeField] private bool enableFourBandProcessing = false;

    [Header("Crossover frequencies")]
    [SerializeField, Range(20f, 20000f)] private float lowCrossover = DefaultLowCrossover;
    [SerializeField, Range(20f, 20000f)] private float midCrossover = DefaultMidCrossover;
    [SerializeField, Range(20f, 20000f)] private float highCrossover = DefaultHighCrossover;

    [Header("Band effects")]
    [SerializeField] private BandSettings low = new BandSettings();
    [SerializeField] private BandSettings lowMid = new BandSettings();
    [SerializeField] private BandSettings highMid = new BandSettings();
    [SerializeField] private BandSettings high = new BandSettings();

    [Header("Movement")]
    [Tooltip("Pan, spatial blend and output gain interpolation time. Set to 0 for immediate changes.")]
    [SerializeField, Range(0f, 100f)] private float motionSmoothingMs = 5f;

    private AudioSource audioSource;

    private void OnEnable()
    {
        ValidateSettings();
        audioSource = GetComponent<AudioSource>();
        audioSource.spatialize = true;
        ApplySettings();
    }

    private void OnValidate() { ValidateSettings(); }

    private void Update()
    {
        // AudioSource가 Spatializer 인스턴스를 생성하거나 재생성한 뒤에도 설정값을 받아오기
        ApplySettings();
    }

    private void OnDisable()
    {
        if (audioSource == null || !audioSource.spatialize) return;
        audioSource.SetSpatializerFloat(EnableIndex, 0f);
        audioSource.SetSpatializerFloat(MotionSmoothingIndex, 5f);
    }

    private void ValidateSettings()
    {
        lowCrossover = FiniteClamp(lowCrossover, 20f, 19998f, DefaultLowCrossover);
        midCrossover = FiniteClamp(midCrossover, lowCrossover + 1f, 19999f, DefaultMidCrossover);
        highCrossover = FiniteClamp(highCrossover, midCrossover + 1f, 20000f, DefaultHighCrossover);
        motionSmoothingMs = FiniteClamp(motionSmoothingMs, 0f, 100f, 5f);
        if (low == null) low = new BandSettings();
        if (lowMid == null) lowMid = new BandSettings();
        if (highMid == null) highMid = new BandSettings();
        if (high == null) high = new BandSettings();
        low.Validate(); lowMid.Validate(); highMid.Validate(); high.Validate();
    }

    private void ApplySettings()
    {
        if (audioSource == null || !audioSource.spatialize) return;
        audioSource.SetSpatializerFloat(LowCrossoverIndex, lowCrossover);
        audioSource.SetSpatializerFloat(LowCrossoverIndex + 1, midCrossover);
        audioSource.SetSpatializerFloat(LowCrossoverIndex + 2, highCrossover);
        ApplyBand(0, low); ApplyBand(1, lowMid); ApplyBand(2, highMid); ApplyBand(3, high);
        audioSource.SetSpatializerFloat(MotionSmoothingIndex, motionSmoothingMs);
        audioSource.SetSpatializerFloat(EnableIndex, enableFourBandProcessing ? 1f : 0f);
    }

    private void ApplyBand(int band, BandSettings settings)
    {
        audioSource.SetSpatializerFloat(BandGainIndex + band, settings.gain);
        audioSource.SetSpatializerFloat(BandDriveIndex + band, settings.saturationDrive);
        audioSource.SetSpatializerFloat(BandWetIndex + band, settings.saturationWet);
    }

    private static float FiniteClamp(float value, float minimum, float maximum, float fallback)
    {
        return Mathf.Clamp(float.IsNaN(value) || float.IsInfinity(value) ? fallback : value, minimum, maximum);
    }
}
