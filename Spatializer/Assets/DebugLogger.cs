using System;
using System.Runtime.InteropServices;
using UnityEngine;

public sealed class DebugLogger : MonoBehaviour
{
    [SerializeField, Min(0.05f)]
    private float logInterval = 0.5f;

    private readonly float[] spatialization = new float[5];
    private float nextLogTime;

    [DllImport("AudioPluginDemo", CallingConvention = CallingConvention.Cdecl)]
    private static extern int Spatializer_GetCurrentSpatialization(
        [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] float[] values,
        int count);

    private void OnEnable()
    {
        nextLogTime = 0f;
    }

    private void Update()
    {
        if (Time.unscaledTime < nextLogTime)
            return;

        nextLogTime = Time.unscaledTime + Mathf.Max(0.05f, logInterval);

        int result;
        try
        {
            result = Spatializer_GetCurrentSpatialization(spatialization, spatialization.Length);
        }
        catch (DllNotFoundException exception)
        {
            DisableWithError("AudioPluginDemo native plugin could not be loaded.", exception);
            return;
        }
        catch (EntryPointNotFoundException exception)
        {
            DisableWithError("Rebuild AudioPluginDemo with Spatializer_GetCurrentSpatialization.", exception);
            return;
        }
        catch (BadImageFormatException exception)
        {
            DisableWithError("AudioPluginDemo must match the Unity process architecture.", exception);
            return;
        }

        // 스냅샷이 없거나 오디오 쓰레드가 이미 값을 저장하고 있는 경우, 여러 개의 오브젝트/카메라가 이를 조회하고 있을 가능성이 있어 조회를 생략함.
        if (result == 0)
            return;

        if (result != 1)
        {
            Debug.LogError($"[Spatializer] Native query failed (result={result}).", this);
            enabled = false;
            return;
        }

        Debug.Log(
            $"[Spatializer] ListenerLocalPosition=({spatialization[0]:F3}, {spatialization[1]:F3}, {spatialization[2]:F3}), " +
            $"Azimuth={spatialization[3]:F2} deg, Elevation={spatialization[4]:F2} deg",
            this);
    }

    private void DisableWithError(string message, Exception exception)
    {
        Debug.LogError($"[Spatializer] {message}\n{exception.Message}", this);
        enabled = false;
    }
}
