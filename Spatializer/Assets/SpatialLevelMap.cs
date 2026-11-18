using System.Threading;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.Rendering;
using Stopwatch = System.Diagnostics.Stopwatch;

[AddComponentMenu("Audio/Spatial Level Map")]
[DisallowMultipleComponent]
[RequireComponent(typeof(AudioSource))]
public sealed class SpatialLevelMap : MonoBehaviour
{
    private const float DisplayInterval = 1f / 20f;
    private const double CaptureTimeout = 0.25;
    private static readonly int DirectField = Shader.PropertyToID("_DirectField");
    private static readonly int TailField = Shader.PropertyToID("_TailField");
    private static readonly int LevelRange = Shader.PropertyToID("_LevelRange");
    private static readonly int AirRadius = Shader.PropertyToID("_AirRadius");
    private static readonly int AirDensity = Shader.PropertyToID("_AirDensity");

    [SerializeField] private Camera viewCamera;
    [SerializeField] private Shader volumeShader;
    [SerializeField] private Mesh volumeCube;
    [SerializeField, Min(1f)] private float radius = 4.6f;
    [SerializeField, Range(0.05f, 2f)] private float density = 0.4f;
    [SerializeField, Range(-90f, -20f)] private float quietLevelDb = -48f;
    [SerializeField, Range(-36f, 0f)] private float loudLevelDb = -18f;
    [SerializeField, Range(0.1f, 4f)] private float decayTime = 1.2f;
    [SerializeField, Range(-48f, 0f)] private float tailLevelDb = -11f;
    [SerializeField] private bool spaceTogglesSource;

    private AudioSource audioSource;
    private Camera renderCamera;
    private Material volumeMaterial;
    private MaterialPropertyBlock volumeProperties;
    private CommandBuffer volumeDraw;
    private float nextUpdate;
    private float previousUpdate;
    private float sourcePower;
    private float tailPower;
    private Vector3 tailPosition;
    private float capturedPower;
    private float pendingPeakPower;
    private long captureTimestamp;
    private bool spaceWasPressed;

    private void OnEnable()
    {
        audioSource = GetComponent<AudioSource>();
        renderCamera = viewCamera != null ? viewCamera : Camera.main;
        if (renderCamera != null && volumeShader != null && volumeShader.isSupported && volumeCube != null)
        {
            volumeMaterial = new Material(volumeShader) { hideFlags = HideFlags.HideAndDontSave };
            volumeProperties ??= new MaterialPropertyBlock();
            volumeDraw = new CommandBuffer { name = "Acoustic level volume" };
            renderCamera.depthTextureMode |= DepthTextureMode.Depth;
            renderCamera.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, volumeDraw);
        }

        Volatile.Write(ref captureTimestamp, 0);
        Interlocked.Exchange(ref pendingPeakPower, 0f);
        sourcePower = tailPower = 0f;
        tailPosition = transform.position;
        spaceWasPressed = Keyboard.current?.spaceKey.isPressed == true;
        previousUpdate = nextUpdate = Time.unscaledTime;
    }

    private void OnDisable()
    {
        if (volumeDraw != null)
        {
            if (renderCamera != null)
                renderCamera.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, volumeDraw);
            volumeDraw.Release();
            volumeDraw = null;
        }
        Destroy(volumeMaterial);
        volumeMaterial = null;
    }

    // Spatialize Post Effects places the measurement ahead of the spatializer.
    // Publish only scalar levels from the audio thread and leave the samples untouched.
    private void OnAudioFilterRead(float[] samples, int channels)
    {
        if (samples.Length == 0)
            return;

        double sum = 0;
        for (int i = 0; i < samples.Length; i++)
        {
            double sample = samples[i];
            sum += sample * sample;
        }

        float power = (float)(sum / samples.Length);
        if (float.IsNaN(power) || float.IsInfinity(power))
            return;

        // Keep brief sounds available until the slower display update consumes them.
        float peak;
        do
        {
            peak = Volatile.Read(ref pendingPeakPower);
            if (power <= peak)
                break;
        }
        while (Interlocked.CompareExchange(ref pendingPeakPower, power, peak) != peak);

        Volatile.Write(ref capturedPower, power);
        Volatile.Write(ref captureTimestamp, Stopwatch.GetTimestamp());
    }

    private void Update()
    {
        bool spacePressed = spaceTogglesSource && Keyboard.current?.spaceKey.isPressed == true;
        if (spacePressed && !spaceWasPressed)
        {
            if (audioSource.isPlaying)
                audioSource.Stop();
            else
                audioSource.Play();
        }
        spaceWasPressed = spacePressed;

        float now = Time.unscaledTime;
        if (now < nextUpdate)
            return;

        RefreshLevels(now - previousUpdate);
        previousUpdate = now;
        nextUpdate = now + DisplayInterval;
    }

    private void RefreshLevels(float elapsed)
    {
        long timestamp = Volatile.Read(ref captureTimestamp);
        bool hasRecentAudio = timestamp != 0 &&
            (Stopwatch.GetTimestamp() - timestamp) / (double)Stopwatch.Frequency < CaptureTimeout;
        float peak = Interlocked.Exchange(ref pendingPeakPower, 0f);
        bool audible = !audioSource.mute && hasRecentAudio;
        if (audioSource.isPlaying && audible)
        {
            float measuredPower = Mathf.Max(Volatile.Read(ref capturedPower), peak);
            sourcePower = Mathf.Lerp(sourcePower, measuredPower, 1f - Mathf.Exp(-elapsed / 0.075f));
        }
        else
        {
            sourcePower = 0f;
        }

        // RT60 reduces mean-square power by 60 dB over decayTime seconds.
        tailPower *= Mathf.Pow(10f, -6f * elapsed / Mathf.Max(0.1f, decayTime));
        float excitation = audible ? Mathf.Max(sourcePower, peak) * Mathf.Pow(10f, tailLevelDb / 10f) : 0f;
        if (excitation > tailPower)
        {
            tailPower = excitation;
            tailPosition = transform.position;
        }
    }

    private void LateUpdate()
    {
        if (volumeDraw == null)
            return;
        volumeDraw.Clear();

        float floorPower = Mathf.Pow(10f, quietLevelDb / 10f);
        if (sourcePower + tailPower <= floorPower)
            return;

        float extent = Mathf.Max(1f, radius);
        Vector3 sourcePosition = transform.position;
        var bounds = new Bounds(sourcePosition, Vector3.one * (2f * extent));
        if (tailPower > 0f)
        {
            Vector3 halfSize = Vector3.one * extent;
            bounds.Encapsulate(tailPosition - halfSize);
            bounds.Encapsulate(tailPosition + halfSize);
        }

        float levelSpan = Mathf.Max(1f, loudLevelDb - quietLevelDb);
        volumeProperties.SetVector(LevelRange, new Vector4(quietLevelDb, 1f / levelSpan, 0f, 0f));
        volumeProperties.SetFloat(AirRadius, extent);
        volumeProperties.SetFloat(AirDensity, density);
        volumeProperties.SetVector(DirectField, new Vector4(sourcePosition.x, sourcePosition.y, sourcePosition.z, sourcePower));
        volumeProperties.SetVector(TailField, new Vector4(tailPosition.x, tailPosition.y, tailPosition.z, tailPower));

        Matrix4x4 placement = Matrix4x4.TRS(bounds.center, Quaternion.identity, bounds.size);
        volumeDraw.DrawMesh(volumeCube, placement, volumeMaterial, 0, 0, volumeProperties);
    }
}
