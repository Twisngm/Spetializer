Shader "Spatializer/Acoustic Volume"
{
    SubShader
    {
        Tags { "Queue" = "Transparent+40" "RenderType" = "Transparent" "IgnoreProjector" = "True" }
        Pass
        {
            Cull Front
            ZWrite Off
            ZTest Always
            Blend One OneMinusSrcAlpha

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex Vert
            #pragma fragment Frag
            #include "UnityCG.cginc"

            UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);
            sampler3D _ViewPower;
            sampler3D _ListenerPower;
            float3 _ViewGridSize;
            float4x4 _SampledViewProjection;
            float4 _SampledEyeRow;
            float3 _SampledDepths;
            float4 _ListenerRegion;
            float3 _VolumeMin;
            float3 _VolumeMax;
            float4 _LevelRange;
            float _AirDensity;

            struct Varyings
            {
                float4 position : SV_POSITION;
                float4 screenPosition : TEXCOORD0;
                float3 worldPosition : TEXCOORD1;
            };

            Varyings Vert(float4 vertex : POSITION)
            {
                Varyings result;
                result.position = UnityObjectToClipPos(vertex);
                result.screenPosition = ComputeScreenPos(result.position);
                result.worldPosition = mul(unity_ObjectToWorld, vertex).xyz;
                return result;
            }

            float2 RayInterval(float3 origin, float3 direction)
            {
                float3 inverseDirection = (2.0 * step(0.0, direction) - 1.0) / max(abs(direction), 1e-6);
                float3 first = (_VolumeMin - origin) * inverseDirection;
                float3 last = (_VolumeMax - origin) * inverseDirection;
                float3 entry = min(first, last);
                float3 exit = max(first, last);
                return float2(max(entry.x, max(entry.y, entry.z)), min(exit.x, min(exit.y, exit.z)));
            }

            float2 SampleMeasuredPower(float3 position)
            {
                float3 listenerOffset = position - _ListenerRegion.xyz;
                float radius = _ListenerRegion.w;
                if (radius > 0.0 && dot(listenerOffset, listenerOffset) <= radius * radius)
                {
                    // One-unit spacing, with half a texel at each side of the cube.
                    float3 uv = listenerOffset / (2.0 * radius + 1.0) + 0.5;
                    return tex3Dlod(_ListenerPower, float4(uv, 0)).rg;
                }

                float4 world = float4(position, 1.0);
                float eyeDepth = -dot(_SampledEyeRow, world);
                if (eyeDepth < _SampledDepths.x || eyeDepth > _SampledDepths.y)
                    return 0.0;

                float4 clip = mul(_SampledViewProjection, world);
                float2 viewport = clip.xy / clip.w * 0.5 + 0.5;
                if (any(viewport < 0.0) || any(viewport > 1.0))
                    return 0.0;

                float3 grid = float3(viewport, (eyeDepth - _SampledDepths.x) * _SampledDepths.z);
                float3 uv = (grid * (_ViewGridSize - 1.0) + 0.5) / _ViewGridSize;
                return tex3Dlod(_ViewPower, float4(uv, 0)).rg;
            }

            float3 LevelColor(float level)
            {
                const float3 blue = float3(0.035, 0.19, 0.88);
                const float3 teal = float3(0.025, 0.67, 0.57);
                const float3 amber = float3(1.0, 0.34, 0.07);
                return level < 0.58
                    ? lerp(blue, teal, level / 0.58)
                    : lerp(teal, amber, (level - 0.58) / 0.42);
            }

            float4 Frag(Varyings input) : SV_Target
            {
                float3 forward = -UNITY_MATRIX_V[2].xyz;
                float3 origin = _WorldSpaceCameraPos;
                float3 direction = normalize(input.worldPosition - origin);
                float rawDepth = SAMPLE_DEPTH_TEXTURE_PROJ(_CameraDepthTexture, UNITY_PROJ_COORD(input.screenPosition));
                float sceneDepth = LinearEyeDepth(rawDepth);

                if (unity_OrthoParams.w > 0.5)
                {
                    direction = forward;
                    origin = input.worldPosition - forward * dot(input.worldPosition - origin, forward);
                    #if defined(UNITY_REVERSED_Z)
                    rawDepth = 1.0 - rawDepth;
                    #endif
                    sceneDepth = lerp(_ProjectionParams.y, _ProjectionParams.z, rawDepth);
                }

                float eyeProjection = max(dot(direction, forward), 0.001);
                float2 interval = RayInterval(origin, direction);
                float start = max(_ProjectionParams.y / eyeProjection, interval.x);
                float end = min(sceneDepth / eyeProjection, interval.y);
                if (end <= start)
                    return 0.0;

                const int maxSteps = 36;
                int steps = (int)clamp(ceil((end - start) / 0.45), 1.0, maxSteps);
                float stride = (end - start) / steps;
                float3 samplePosition = origin + direction * (start + stride * 0.5);
                float4 accumulated = 0.0;
                [loop]
                for (int i = 0; i < steps; i++)
                {
                    float2 measurement = SampleMeasuredPower(samplePosition);
                    // Divide out the interpolated mask to ignore unsampled texels.
                    float power = measurement.x / max(0.001, measurement.y);
                    float db = 10.0 * log10(max(1e-12, power));
                    float level = saturate((db - _LevelRange.x) * _LevelRange.y);
                    float opacity = 1.0 - exp(-_AirDensity * level * level * level * measurement.y * stride);
                    float weight = (1.0 - accumulated.a) * opacity;
                    accumulated += float4(LevelColor(level) * weight, weight);
                    samplePosition += direction * stride;
                    if (accumulated.a >= 0.98)
                        break;
                }
                return accumulated;
            }
            ENDCG
        }
    }
    Fallback Off
}
