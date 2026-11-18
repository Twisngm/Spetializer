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
            float4 _DirectField;
            float4 _TailField;
            float4 _LevelRange;
            float _AirRadius;
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

            float2 RayInterval(float3 origin, float3 direction, float4 field)
            {
                float3 offset = origin - field.xyz;
                float along = dot(offset, direction);
                float discriminant = along * along - dot(offset, offset) + _AirRadius * _AirRadius;
                if (field.w <= 0.0 || discriminant <= 0.0)
                    return float2(1e8, -1e8);

                float halfChord = sqrt(discriminant);
                return float2(-along - halfChord, -along + halfChord);
            }

            float PowerAt(float3 position, float4 field)
            {
                float3 offset = position - field.xyz;
                float distanceSquared = dot(offset, offset);
                float radiusFractionSquared = distanceSquared / (_AirRadius * _AirRadius);
                float edge = 1.0 - smoothstep(0.48, 1.0, radiusFractionSquared);
                return field.w * edge / max(1.0, distanceSquared);
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
                float2 directInterval = RayInterval(origin, direction, _DirectField);
                float2 tailInterval = RayInterval(origin, direction, _TailField);
                float start = max(_ProjectionParams.y / eyeProjection, min(directInterval.x, tailInterval.x));
                float end = min(sceneDepth / eyeProjection, max(directInterval.y, tailInterval.y));
                if (end <= start)
                    return 0.0;

                const int maxSteps = 28;
                int steps = (int)clamp(ceil((end - start) / 0.42), 1.0, maxSteps);
                float stride = (end - start) / steps;
                float3 samplePosition = origin + direction * (start + stride * 0.5);
                float4 accumulated = 0.0;
                [loop]
                for (int i = 0; i < steps; i++)
                {
                    float power = PowerAt(samplePosition, _DirectField) + PowerAt(samplePosition, _TailField);
                    float db = 10.0 * log10(max(1e-12, power));
                    float level = saturate((db - _LevelRange.x) * _LevelRange.y);
                    float opacity = 1.0 - exp(-_AirDensity * level * level * stride);
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
