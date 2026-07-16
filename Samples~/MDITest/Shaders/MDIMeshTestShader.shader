Shader "Hidden/MDIMeshTest"
{
    SubShader
    {
        Tags { "RenderType" = "Opaque" "RenderPipeline" = "UniversalPipeline" }

        // ---------------------------------------------------------------
        // Pass 0: Graphics.RenderMeshIndirect (Unity built-in)
        //
        // Tagged UniversalForward so URP picks this pass automatically when
        // the test controller calls Graphics.RenderMeshIndirect.
        //
        // The mesh path needs its own UniversalForward pass — the procedural
        // MDITestShader already owns one and URP only ever picks the first
        // matching pass per material.
        // ---------------------------------------------------------------
        Pass
        {
            Name "RenderMeshIndirectForward"
            Tags { "LightMode" = "UniversalForward" }

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex vert
            #pragma fragment frag

            #define UNITY_INDIRECT_DRAW_ARGS IndirectDrawIndexedArgs
            #include "UnityIndirect.cginc"
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            struct MeshAttributes
            {
                float4 positionOS : POSITION;
                float3 normalOS   : NORMAL;
            };

            struct VertexOutput
            {
                float4 positionCS : SV_POSITION;
                float3 color      : TEXCOORD0;
            };

            StructuredBuffer<float3> _DrawPositions;

            VertexOutput vert(MeshAttributes input, uint svInstanceID : SV_InstanceID)
            {
                InitIndirectDrawArgs(0);

                // Vulkan / WebGPU: SV_InstanceID already includes the indirect
                // args' startInstance (see GetIndirectInstanceID_Base in
                // UnityIndirect.cginc) — adding it again would double the offset.
                #if defined(SHADER_API_VULKAN) || defined(SHADER_API_WEBGPU)
                uint globalInstanceID = svInstanceID;
                #else
                uint globalInstanceID = (globalIndirectDrawArgs.startInstance + svInstanceID);
                #endif

                VertexOutput o;
                float t = (float)globalInstanceID / 64.0;

                float3 worldPos = input.positionOS.xyz
                                + _DrawPositions[globalInstanceID]
                                + float3(0, sin(_Time.y + t), 0);

                o.positionCS = TransformWorldToHClip(worldPos);

                float3 instanceTint = float3(frac(t * 3.0), frac(t * 5.0), frac(t * 7.0));
                float ndotl = saturate(dot(normalize(input.normalOS), normalize(float3(0.4, 1.0, 0.3))));
                o.color = instanceTint * (0.4 + 0.6 * ndotl);
                return o;
            }

            float4 frag(VertexOutput i) : SV_Target
            {
                return float4(i.color, 1.0);
            }
            ENDHLSL
        }
    }
}
