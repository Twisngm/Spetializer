using System;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.Rendering;

[AddComponentMenu("Audio/Spatial Level Map")]
[DisallowMultipleComponent]
[RequireComponent(typeof(AudioSource))]
public sealed class SpatialLevelMap : MonoBehaviour
{
    private const float DisplayInterval = 1f / 20f;
    private const float CaptureTimeout = 0.25f;
    private const int ProbeTokenParameter = 22;
    private const int ViewWidth = 11;
    private const int ViewHeight = 7;
    private const int ViewSlices = 9;
    private const int ViewCells = ViewWidth * ViewHeight * ViewSlices;
    private const int ListenerRadius = 3;
    private const int ListenerSide = ListenerRadius * 2 + 1;
    private const int ListenerCells = ListenerSide * ListenerSide * ListenerSide;
    private const int MaxPoints = ViewCells + ListenerCells;
    private static readonly int ViewPower = Shader.PropertyToID("_ViewPower");
    private static readonly int ViewGridSize = Shader.PropertyToID("_ViewGridSize");
    private static readonly int ListenerPower = Shader.PropertyToID("_ListenerPower");
    private static readonly int SampledViewProjection = Shader.PropertyToID("_SampledViewProjection");
    private static readonly int SampledEyeRow = Shader.PropertyToID("_SampledEyeRow");
    private static readonly int SampledDepths = Shader.PropertyToID("_SampledDepths");
    private static readonly int ListenerRegion = Shader.PropertyToID("_ListenerRegion");
    private static readonly int VolumeMin = Shader.PropertyToID("_VolumeMin");
    private static readonly int VolumeMax = Shader.PropertyToID("_VolumeMax");
    private static readonly int LevelRange = Shader.PropertyToID("_LevelRange");
    private static readonly int AirDensity = Shader.PropertyToID("_AirDensity");

    [SerializeField] private Camera viewCamera;
    [SerializeField] private AudioListener listener;
    [SerializeField] private Shader volumeShader;
    [SerializeField] private Mesh volumeCube;
    [SerializeField, Min(1f)] private float viewProbeDepth = 16f;
    [SerializeField, Range(0.05f, 2f)] private float density = 0.4f;
    [SerializeField, Range(-90f, -20f)] private float quietLevelDb = -48f;
    [SerializeField, Range(-36f, 0f)] private float loudLevelDb = -18f;
    [SerializeField] private bool spaceTogglesSource;

    private readonly float[] positions = new float[MaxPoints * 3];
    private readonly int[] destinations = new int[MaxPoints];
    private readonly float[] powers = new float[MaxPoints];
    private readonly float[] viewValues = new float[ViewCells * 2];
    private readonly float[] listenerValues = new float[ListenerCells * 2];
    private AudioSource audioSource;
    private Camera renderCamera;
    private Material volumeMaterial;
    private MaterialPropertyBlock volumeProperties;
    private CommandBuffer volumeDraw;
    private Texture3D viewTexture;
    private Texture3D listenerTexture;
    private int probeToken;
    private uint revision;
    private float nextUpdate;
    private float lastMeasurement;
    private Matrix4x4 measuredViewProjection;
    private Vector4 measuredEyeRow;
    private Vector4 measuredDepths;
    private Vector4 measuredListener;
    private Bounds measuredBounds;
    private float peakPower;
    private bool hasMeasurement;
    private bool spaceWasPressed;

    [DllImport("AudioPluginDemo", CallingConvention = CallingConvention.Cdecl)]
    private static extern int Spatializer_CreateLevelProbe();

    [DllImport("AudioPluginDemo", CallingConvention = CallingConvention.Cdecl)]
    private static extern void Spatializer_DestroyLevelProbe(int token);

    [DllImport("AudioPluginDemo", CallingConvention = CallingConvention.Cdecl)]
    private static extern int Spatializer_ReadLevelProbePoints(int token, ref uint revision,
        [In] float[] positions, [Out] float[] powers, int count);

    private void OnEnable()
    {
        audioSource = GetComponent<AudioSource>();
        renderCamera = viewCamera != null ? viewCamera : Camera.main;
        if (listener == null)
            listener = FindFirstObjectByType<AudioListener>();
        if (renderCamera != null && volumeShader != null && volumeShader.isSupported && volumeCube != null &&
            SystemInfo.supports3DTextures && SystemInfo.SupportsTextureFormat(TextureFormat.RGFloat))
        {
            try
            {
                probeToken = Spatializer_CreateLevelProbe();
            }
            catch (Exception exception) when (exception is DllNotFoundException ||
                exception is EntryPointNotFoundException || exception is BadImageFormatException)
            {
                Debug.LogWarning($"[Spatializer] The native level probe could not be loaded. {exception.Message}", this);
            }

            if (probeToken != 0)
            {
                viewTexture = CreateProbeTexture(ViewWidth, ViewHeight, ViewSlices, "View probe powers");
                listenerTexture = CreateProbeTexture(ListenerSide, ListenerSide, ListenerSide, "Listener probe powers");
                volumeMaterial = new Material(volumeShader) { hideFlags = HideFlags.HideAndDontSave };
                volumeProperties ??= new MaterialPropertyBlock();
                volumeDraw = new CommandBuffer { name = "Acoustic level volume" };
                renderCamera.depthTextureMode |= DepthTextureMode.Depth;
                renderCamera.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, volumeDraw);
            }
        }

        revision = 0;
        hasMeasurement = false;
        spaceWasPressed = Keyboard.current?.spaceKey.isPressed == true;
        nextUpdate = Time.unscaledTime;
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
        if (probeToken != 0)
        {
            if (audioSource != null && audioSource.spatialize)
                audioSource.SetSpatializerFloat(ProbeTokenParameter, 0f);
            Spatializer_DestroyLevelProbe(probeToken);
            probeToken = 0;
        }
        Destroy(viewTexture);
        Destroy(listenerTexture);
        Destroy(volumeMaterial);
        viewTexture = listenerTexture = null;
        volumeMaterial = null;
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

        if (probeToken == 0 || renderCamera == null || !audioSource.isPlaying || !audioSource.spatialize || audioSource.mute)
        {
            hasMeasurement = false;
            return;
        }
        float now = Time.unscaledTime;
        if (now < nextUpdate)
            return;

        nextUpdate = now + DisplayInterval;
        if (!audioSource.SetSpatializerFloat(ProbeTokenParameter, probeToken))
        {
            hasMeasurement = false;
            return;
        }

        if (MeasureVisibleAndListenerProbes())
        {
            lastMeasurement = now;
            hasMeasurement = true;
        }
        else if (now - lastMeasurement > CaptureTimeout)
        {
            hasMeasurement = false;
        }
    }

    private static Texture3D CreateProbeTexture(int width, int height, int depth, string textureName)
    {
        return new Texture3D(width, height, depth, TextureFormat.RGFloat, false)
        {
            name = textureName,
            filterMode = FilterMode.Bilinear,
            wrapMode = TextureWrapMode.Clamp
        };
    }

    private bool MeasureVisibleAndListenerProbes()
    {
        float near = renderCamera.nearClipPlane;
        float far = Mathf.Min(renderCamera.farClipPlane, Mathf.Max(near + 0.01f, viewProbeDepth));
        bool hasListener = listener != null && listener.isActiveAndEnabled;
        Vector3 listenerPosition = hasListener ? listener.transform.position : Vector3.zero;
        Vector3 nearCorner = renderCamera.ViewportToWorldPoint(new Vector3(0f, 0f, near));
        Vector3 farCorner = renderCamera.ViewportToWorldPoint(new Vector3(0f, 0f, far));
        Vector3 nearRight = renderCamera.ViewportToWorldPoint(new Vector3(1f, 0f, near)) - nearCorner;
        Vector3 farRight = renderCamera.ViewportToWorldPoint(new Vector3(1f, 0f, far)) - farCorner;
        Vector3 nearUp = renderCamera.ViewportToWorldPoint(new Vector3(0f, 1f, near)) - nearCorner;
        Vector3 farUp = renderCamera.ViewportToWorldPoint(new Vector3(0f, 1f, far)) - farCorner;
        var bounds = new Bounds(nearCorner, Vector3.zero);
        bounds.Encapsulate(nearCorner + nearRight);
        bounds.Encapsulate(nearCorner + nearUp);
        bounds.Encapsulate(nearCorner + nearRight + nearUp);
        bounds.Encapsulate(farCorner);
        bounds.Encapsulate(farCorner + farRight);
        bounds.Encapsulate(farCorner + farUp);
        bounds.Encapsulate(farCorner + farRight + farUp);

        int count = 0;
        for (int z = 0; z < ViewSlices; z++)
        {
            float depthFraction = z / (float)(ViewSlices - 1);
            Vector3 corner = Vector3.Lerp(nearCorner, farCorner, depthFraction);
            Vector3 stepX = Vector3.Lerp(nearRight, farRight, depthFraction) / (ViewWidth - 1);
            Vector3 stepY = Vector3.Lerp(nearUp, farUp, depthFraction) / (ViewHeight - 1);
            for (int y = 0; y < ViewHeight; y++)
                for (int x = 0; x < ViewWidth; x++)
                {
                    Vector3 point = corner + x * stepX + y * stepY;
                    if (hasListener && (point - listenerPosition).sqrMagnitude <= ListenerRadius * ListenerRadius)
                        continue;
                    AddPoint(ref count, point, x + ViewWidth * (y + ViewHeight * z));
                }
        }

        if (hasListener)
        {
            Vector3 extent = Vector3.one * ListenerRadius;
            bounds.Encapsulate(listenerPosition - extent);
            bounds.Encapsulate(listenerPosition + extent);
            for (int z = -ListenerRadius; z <= ListenerRadius; z++)
                for (int y = -ListenerRadius; y <= ListenerRadius; y++)
                    for (int x = -ListenerRadius; x <= ListenerRadius; x++)
                    {
                        if (x * x + y * y + z * z > ListenerRadius * ListenerRadius)
                            continue;
                        int index = x + ListenerRadius + ListenerSide * (y + ListenerRadius + ListenerSide * (z + ListenerRadius));
                        AddPoint(ref count, listenerPosition + new Vector3(x, y, z), ViewCells + index);
                    }
        }

        if (Spatializer_ReadLevelProbePoints(probeToken, ref revision, positions, powers, count) != 1)
            return false;

        // The second channel masks unsampled texels so interpolation excludes them.
        Array.Clear(viewValues, 0, viewValues.Length);
        Array.Clear(listenerValues, 0, listenerValues.Length);
        peakPower = 0f;
        for (int i = 0; i < count; i++)
        {
            int destination = destinations[i];
            float[] values = destination < ViewCells ? viewValues : listenerValues;
            int offset = 2 * (destination < ViewCells ? destination : destination - ViewCells);
            values[offset] = powers[i];
            values[offset + 1] = 1f;
            peakPower = Mathf.Max(peakPower, powers[i]);
        }
        viewTexture.SetPixelData(viewValues, 0);
        viewTexture.Apply(false);
        listenerTexture.SetPixelData(listenerValues, 0);
        listenerTexture.Apply(false);
        Matrix4x4 view = renderCamera.worldToCameraMatrix;
        measuredViewProjection = renderCamera.projectionMatrix * view;
        measuredEyeRow = view.GetRow(2);
        measuredDepths = new Vector4(near, far, 1f / (far - near), 0f);
        measuredListener = new Vector4(listenerPosition.x, listenerPosition.y, listenerPosition.z, hasListener ? ListenerRadius : 0f);
        measuredBounds = bounds;
        return true;
    }

    private void AddPoint(ref int count, Vector3 point, int destination)
    {
        int offset = count * 3;
        positions[offset] = point.x;
        positions[offset + 1] = point.y;
        positions[offset + 2] = point.z;
        destinations[count++] = destination;
    }

    private void LateUpdate()
    {
        if (volumeDraw == null)
            return;
        volumeDraw.Clear();

        float floorPower = Mathf.Pow(10f, quietLevelDb / 10f);
        if (!hasMeasurement || peakPower <= floorPower)
            return;

        float levelSpan = Mathf.Max(1f, loudLevelDb - quietLevelDb);
        volumeProperties.SetVector(LevelRange, new Vector4(quietLevelDb, 1f / levelSpan, 0f, 0f));
        volumeProperties.SetFloat(AirDensity, density);
        volumeProperties.SetTexture(ViewPower, viewTexture);
        volumeProperties.SetVector(ViewGridSize, new Vector3(ViewWidth, ViewHeight, ViewSlices));
        volumeProperties.SetTexture(ListenerPower, listenerTexture);
        volumeProperties.SetMatrix(SampledViewProjection, measuredViewProjection);
        volumeProperties.SetVector(SampledEyeRow, measuredEyeRow);
        volumeProperties.SetVector(SampledDepths, measuredDepths);
        volumeProperties.SetVector(ListenerRegion, measuredListener);
        volumeProperties.SetVector(VolumeMin, measuredBounds.min);
        volumeProperties.SetVector(VolumeMax, measuredBounds.max);

        Matrix4x4 placement = Matrix4x4.TRS(measuredBounds.center, Quaternion.identity, measuredBounds.size);
        volumeDraw.DrawMesh(volumeCube, placement, volumeMaterial, 0, 0, volumeProperties);
    }
}
