using UnityEngine;

[DisallowMultipleComponent]
[RequireComponent(typeof(AudioSource))]
public sealed class SpatializerGain : MonoBehaviour
{
    // Plugin_Spatializer.cpp의 P_GAIN값과 연동해야 함.
    private const int GainParameterIndex = 3;

    [SerializeField, Range(0f, 4f)]
    [Tooltip("Linear output gain: 0 = mute, 1 = original level, 2 = double amplitude.")]
    private float gain = 2f;

    private AudioSource audioSource;

    private void OnEnable()
    {
        audioSource = GetComponent<AudioSource>();
        audioSource.spatialize = true;
        ApplyGain();
    }

    private void OnValidate()
    {
        gain = Mathf.Clamp(gain, 0f, 4f);
    }

    private void Update()
    {
        ApplyGain(); // 임시로 프레임 단위 정보 갱신하도록 설정
    }

    private void ApplyGain()
    {
        if (audioSource == null || !audioSource.spatialize)
            return; // Native에서 파라미터를 받을 수 없으면 Return


        audioSource.SetSpatializerFloat(GainParameterIndex, gain);
    }
}
