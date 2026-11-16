using UnityEngine;

[AddComponentMenu("Audio/Spatializer Distance Attenuation")]
[DisallowMultipleComponent]
[RequireComponent(typeof(AudioSource))]
public sealed class SpatializerDistanceAttenuation : MonoBehaviour
{
    // P_FREE_FIELD_ATTENUATION in SpatializerParameters.h.
    private const int FreeFieldAttenuationIndex = 21;

    private AudioSource audioSource;

    private void OnEnable()
    {
        audioSource = GetComponent<AudioSource>();
        SetFreeFieldAttenuation(true);
    }

    private void Update()
    {
        // The native spatializer may be created or recreated after OnEnable.
        SetFreeFieldAttenuation(true);
    }

    private void OnDisable()
    {
        SetFreeFieldAttenuation(false);
    }

    private void SetFreeFieldAttenuation(bool enabled)
    {
        if (audioSource != null && audioSource.spatialize)
            audioSource.SetSpatializerFloat(FreeFieldAttenuationIndex, enabled ? 1f : 0f);
    }
}
