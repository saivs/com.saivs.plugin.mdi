Shader "Hidden/MDIIndexedTest"
{
    SubShader
    {
        Tags { "RenderType" = "Opaque" "RenderPipeline" = "UniversalPipeline" }

        // ---------------------------------------------------------------
        // Shared code across all passes
        // ---------------------------------------------------------------
        HLSLINCLUDE
        #pragma target 4.5
        #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

        struct VertexOutput
        {
            float4 positionCS : SV_POSITION;
            float3 color : TEXCOORD0;
        };

        // Layout matches the C# `MergedVertex` struct in MDITestBufferManager —
        // positions and normals merged from every mesh in the array.
        struct MergedVertex
        {
            float3 position;
            float3 normal;
        };

        StructuredBuffer<MergedVertex> _VertexBuffer;
        StructuredBuffer<float3> _DrawPositions;

        VertexOutput BuildOutput(uint vertexID, uint globalInstanceID)
        {
            VertexOutput o;
            float t = (float)globalInstanceID / 64.0;

            MergedVertex v = _VertexBuffer[vertexID];
            float3 worldPos = v.position
                            + _DrawPositions[globalInstanceID]
                            + float3(0, sin(_Time.y + t), 0);
            o.positionCS = TransformWorldToHClip(worldPos);

            float3 instanceTint = float3(frac(t * 3.0), frac(t * 5.0), frac(t * 7.0));
            float ndotl = saturate(dot(normalize(v.normal), normalize(float3(0.4, 1.0, 0.3))));
            o.color = instanceTint * (0.4 + 0.6 * ndotl);
            return o;
        }

        // Mesh-mode helpers — used by passes that pull vertex data through
        // the input assembler (POSITION/NORMAL semantics) rather than from
        // _VertexBuffer.
        struct MeshAttributes
        {
            float4 positionOS : POSITION;
            float3 normalOS   : NORMAL;
        };

        VertexOutput BuildMeshOutput(MeshAttributes input, uint globalInstanceID)
        {
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

        // ---------------------------------------------------------------
        // Pass 0: RenderPrimitivesIndexedIndirect (Unity built-in)
        // Tagged UniversalForward so URP picks this pass automatically.
        // ---------------------------------------------------------------
        Pass
        {
            Name "RenderPrimitivesForward"
            Tags { "LightMode" = "UniversalForward" }

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            #define UNITY_INDIRECT_DRAW_ARGS IndirectDrawIndexedArgs
            #include "UnityIndirect.cginc"

            VertexOutput vert(uint svVertexID : SV_VertexID, uint svInstanceID : SV_InstanceID)
            {
                InitIndirectDrawArgs(0);

                #if defined(SHADER_API_VULKAN)
                uint globalInstanceID = svInstanceID;
                #else
                uint globalInstanceID = (globalIndirectDrawArgs.startInstance + svInstanceID);
                #endif

                return BuildOutput(svVertexID, globalInstanceID);
            }

            ENDHLSL
        }

        // ---------------------------------------------------------------
        // Pass 1: MultiDrawIndexedIndirect (native plugin, indexed path)
        // ---------------------------------------------------------------
        Pass
        {
            Name "MDIIndexedForward"

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            #include "Packages/com.saivs.multi-draw-indirect/Runtime/ShaderLibrary/MDI.hlsl"

            VertexOutput vert(uint vertexID : SV_VertexID, MDI_INSTANCE_ID_PARAMETER)
            {
                uint globalInstanceID = MDI_INSTANCE_ID;
                return BuildOutput(vertexID, globalInstanceID);
            }

            ENDHLSL
        }

        // ---------------------------------------------------------------
        // Pass 2: cmd.DrawProceduralIndirect loop (managed loop, indexed)
        // C# sets _DrawCallIndex via cmd.SetGlobalInt before each draw.
        // ---------------------------------------------------------------
        Pass
        {
            Name "ProceduralLoopForward"

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            struct IndirectDrawIndexedArgs
            {
                uint indexCountPerInstance;
                uint instanceCount;
                uint startIndex;
                uint baseVertexIndex;
                uint startInstance;
            };

            StructuredBuffer<IndirectDrawIndexedArgs> _ArgsBuffer;
            int _DrawCallIndex;

            VertexOutput vert(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
            {
                // Vulkan / WebGPU: SV_InstanceID already includes the indirect
                // args' startInstance — adding it again would double the offset.
                #if defined(SHADER_API_VULKAN) || defined(SHADER_API_WEBGPU)
                uint globalInstanceID = instanceID;
                #else
                uint globalInstanceID = _ArgsBuffer[_DrawCallIndex].startInstance + instanceID;
                #endif

                return BuildOutput(vertexID, globalInstanceID);
            }
            ENDHLSL
        }

        // ---------------------------------------------------------------
        // Pass 3: MultiDrawMeshIndirect (native plugin, mesh path)
        // Vertices come through the standard POSITION/NORMAL semantics.
        // ---------------------------------------------------------------
        Pass
        {
            Name "MDIMeshForward"

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            #include "Packages/com.saivs.multi-draw-indirect/Runtime/ShaderLibrary/MDI.hlsl"

            VertexOutput vert(MeshAttributes input, MDI_INSTANCE_ID_PARAMETER)
            {
                uint globalInstanceID = MDI_INSTANCE_ID;
                return BuildMeshOutput(input, globalInstanceID);
            }
            ENDHLSL
        }

        // ---------------------------------------------------------------
        // Pass 4: cmd.DrawMeshInstancedIndirect loop (managed loop, mesh)
        // ---------------------------------------------------------------
        Pass
        {
            Name "DrawMeshInstancedIndirectForward"

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            struct IndirectDrawIndexedArgs
            {
                uint indexCountPerInstance;
                uint instanceCount;
                uint startIndex;
                uint baseVertexIndex;
                uint startInstance;
            };

            StructuredBuffer<IndirectDrawIndexedArgs> _ArgsBuffer;
            int _DrawCallIndex;

            VertexOutput vert(MeshAttributes input, uint instanceID : SV_InstanceID)
            {
                // Vulkan / WebGPU: SV_InstanceID already includes the indirect
                // args' startInstance — adding it again would double the offset.
                #if defined(SHADER_API_VULKAN) || defined(SHADER_API_WEBGPU)
                uint globalInstanceID = instanceID;
                #else
                uint globalInstanceID = _ArgsBuffer[_DrawCallIndex].startInstance + instanceID;
                #endif

                return BuildMeshOutput(input, globalInstanceID);
            }
            ENDHLSL
        }
    }
}
