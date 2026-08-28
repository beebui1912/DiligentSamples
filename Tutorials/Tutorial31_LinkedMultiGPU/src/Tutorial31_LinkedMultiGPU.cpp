/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include <algorithm>

#include "Tutorial31_LinkedMultiGPU.hpp"
#include "MapHelper.hpp"
#include "GraphicsAccessories.hpp"
#include "ColorConversion.h"

namespace Diligent
{

SampleBase* CreateSample()
{
    return new Tutorial31_LinkedMultiGPU();
}

namespace
{

// Cube geometry (position + color), matching the input layout below.
struct CubeVertex
{
    float3 Pos;
    float4 Color;
};

// clang-format off
constexpr CubeVertex CubeVerts[8] =
{
    {float3{-1, -1, -1}, float4{1, 0, 0, 1}},
    {float3{-1, +1, -1}, float4{0, 1, 0, 1}},
    {float3{+1, +1, -1}, float4{0, 0, 1, 1}},
    {float3{+1, -1, -1}, float4{1, 1, 1, 1}},
    {float3{-1, -1, +1}, float4{1, 1, 0, 1}},
    {float3{-1, +1, +1}, float4{0, 1, 1, 1}},
    {float3{+1, +1, +1}, float4{1, 0, 1, 1}},
    {float3{+1, -1, +1}, float4{0.2f, 0.2f, 0.2f, 1}},
};

constexpr Uint32 CubeIndices[36] =
{
    2,0,1, 2,3,0,
    4,6,5, 4,7,6,
    0,7,4, 0,3,7,
    1,0,4, 1,4,5,
    1,5,2, 5,6,2,
    3,6,7, 3,2,6
};
// clang-format on

// Scene shaders. The cube is rendered into a per-view off-screen render target.
const char* g_CubeVSSource = R"(
cbuffer Constants
{
    float4x4 g_WorldViewProj;
    float4   g_Tint;
};

struct VSInput
{
    float3 Pos   : ATTRIB0;
    float4 Color : ATTRIB1;
};

struct PSInput
{
    float4 Pos   : SV_POSITION;
    float4 Color : COLOR0;
};

void main(in VSInput VSIn, out PSInput PSIn)
{
    PSIn.Pos   = mul(float4(VSIn.Pos, 1.0), g_WorldViewProj);
    PSIn.Color = VSIn.Color * g_Tint;
}
)";

const char* g_CubePSSource = R"(
struct PSInput
{
    float4 Pos   : SV_POSITION;
    float4 Color : COLOR0;
};

struct PSOutput
{
    float4 Color : SV_TARGET;
};

void main(in PSInput PSIn, out PSOutput PSOut)
{
    PSOut.Color = PSIn.Color;
}
)";

// Composition shaders. A full-screen triangle samples one view's render target
// and writes it into the corresponding strip of the swap chain back buffer.
const char* g_CompositeVSSource = R"(
struct PSInput
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

void main(in uint VertId : SV_VertexID, out PSInput PSIn)
{
    float2 uv  = float2((VertId << 1) & 2, VertId & 2);
    PSIn.UV    = uv;
    PSIn.Pos   = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

const char* g_CompositePSSource = R"(
Texture2D    g_Tex;
SamplerState g_Tex_sampler;

struct PSInput
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

struct PSOutput
{
    float4 Color : SV_TARGET;
};

void main(in PSInput PSIn, out PSOutput PSOut)
{
    float4 Color = g_Tex.Sample(g_Tex_sampler, PSIn.UV);
#if CONVERT_PS_OUTPUT_TO_GAMMA
    Color.rgb = pow(Color.rgb, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
#endif
    PSOut.Color = Color;
}
)";

} // namespace

void Tutorial31_LinkedMultiGPU::ModifyEngineInitInfo(const ModifyEngineInitInfoAttribs& Attribs)
{
    SampleBase::ModifyEngineInitInfo(Attribs);

    m_NumViews        = 1;
    m_bLinkedMultiGPU = false;

    // Linked multi-GPU (device group) is only supported by the D3D12 and Vulkan backends.
    if (Attribs.DeviceType != RENDER_DEVICE_TYPE_D3D12 && Attribs.DeviceType != RENDER_DEVICE_TYPE_VULKAN)
        return;

    Uint32 NumAdapters = 0;
    Attribs.pFactory->EnumerateAdapters(Attribs.EngineCI.GraphicsAPIVersion, NumAdapters, nullptr);
    if (NumAdapters == 0)
        return;

    std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
    Attribs.pFactory->EnumerateAdapters(Attribs.EngineCI.GraphicsAPIVersion, NumAdapters, Adapters.data());

    Uint32 AdapterId = Attribs.EngineCI.AdapterId;
    if (AdapterId == DEFAULT_ADAPTER_ID || AdapterId >= NumAdapters)
        AdapterId = 0;

    const GraphicsAdapterInfo& AdapterInfo = Adapters[AdapterId];
    if (AdapterInfo.NodeCount <= 1)
    {
        LOG_INFO_MESSAGE("Tutorial31: adapter '", AdapterInfo.Description, "' reports a single GPU node. "
                         "Running in single-GPU mode (the whole scene is rendered on one node).");
        return;
    }

    // Find a graphics queue to drive each node's immediate context.
    Uint8 GraphicsQueueId = DEFAULT_QUEUE_ID;
    for (Uint32 q = 0; q < AdapterInfo.NumQueues; ++q)
    {
        if ((AdapterInfo.Queues[q].QueueType & COMMAND_QUEUE_TYPE_PRIMARY_MASK) == COMMAND_QUEUE_TYPE_GRAPHICS)
        {
            GraphicsQueueId = static_cast<Uint8>(q);
            break;
        }
    }
    if (GraphicsQueueId == DEFAULT_QUEUE_ID)
        GraphicsQueueId = 0;

    m_NumViews        = std::min(AdapterInfo.NodeCount, MaxViews);
    m_bLinkedMultiGPU = true;
    m_AdapterNodeMask = AdapterInfo.NodeMask;

    static const char* const ContextNames[MaxViews] = {"Node 0 (Graphics)", "Node 1 (Graphics)"};

    m_ContextCI.resize(m_NumViews);
    for (Uint32 v = 0; v < m_NumViews; ++v)
    {
        ImmediateContextCreateInfo& Ctx = m_ContextCI[v];
        Ctx.Name      = ContextNames[v];
        Ctx.QueueId   = GraphicsQueueId;
        Ctx.Priority  = QUEUE_PRIORITY_MEDIUM;
        Ctx.NodeIndex = static_cast<Uint8>(v);
    }

    Attribs.EngineCI.GpuMode                = GPU_MODE_LINKED;
    Attribs.EngineCI.NodeCount              = static_cast<Uint8>(m_NumViews);
    Attribs.EngineCI.pImmediateContextInfo  = m_ContextCI.data();
    Attribs.EngineCI.NumImmediateContexts   = m_NumViews;
    // A general fence is needed to synchronize the nodes on the GPU timeline.
    Attribs.EngineCI.Features.NativeFence   = DEVICE_FEATURE_STATE_OPTIONAL;

    LOG_INFO_MESSAGE("Tutorial31: adapter '", AdapterInfo.Description, "' exposes ", AdapterInfo.NodeCount,
                     " linked GPU nodes. Enabling ", GetGpuModeString(GPU_MODE_LINKED), " multi-GPU with ",
                     m_NumViews, " views.");
}

void Tutorial31_LinkedMultiGPU::Initialize(const SampleInitInfo& InitInfo)
{
    SampleBase::Initialize(InitInfo);

    // Capture the per-node immediate contexts. SampleBase only stores context 0, so we grab
    // the additional linked-node contexts ourselves.
    m_NumViews        = std::min(InitInfo.NumImmediateCtx, MaxViews);
    m_bLinkedMultiGPU = m_NumViews > 1;
    for (Uint32 v = 0; v < m_NumViews; ++v)
        m_ViewContexts[v] = InitInfo.ppContexts[v];

    m_AdapterNodeMask = m_bLinkedMultiGPU ? m_pDevice->GetAdapterInfo().NodeMask : 1u;

    const SwapChainDesc& SCDesc = m_pSwapChain->GetDesc();
    m_ColorFmt                  = SCDesc.ColorBufferFormat;
    m_DepthFmt                  = SCDesc.DepthBufferFormat != TEX_FORMAT_UNKNOWN ? SCDesc.DepthBufferFormat : TEX_FORMAT_D32_FLOAT;

    CreateCubeBuffers();
    CreateCubePSO();
    CreateCompositePSO();

    if (m_bLinkedMultiGPU)
    {
        for (Uint32 v = 0; v < m_NumViews; ++v)
        {
            FenceDesc FenceCI;
            FenceCI.Name = "Linked multi-GPU view fence";
            FenceCI.Type = FENCE_TYPE_GENERAL;
            m_pDevice->CreateFence(FenceCI, &m_ViewFence[v]);
        }
    }

    CreateViewRenderTargets(SCDesc.Width, SCDesc.Height);

    IRenderDevice* AppDevices[] = {m_pDevice};
    m_GpuInfoPanel.Initialize(m_pEngineFactory, AppDevices, 1);
}

void Tutorial31_LinkedMultiGPU::CreateCubeBuffers()
{
    const Uint32 SharedCreationMask = m_bLinkedMultiGPU ? 1u : 0u;
    const Uint32 SharedVisibleMask  = m_bLinkedMultiGPU ? m_AdapterNodeMask : 0u;

    {
        BufferDesc VertBuffDesc;
        VertBuffDesc.Name             = "Cube vertex buffer";
        VertBuffDesc.Usage            = USAGE_IMMUTABLE;
        VertBuffDesc.BindFlags        = BIND_VERTEX_BUFFER;
        VertBuffDesc.Size             = sizeof(CubeVerts);
        VertBuffDesc.CreationNodeMask = SharedCreationMask;
        VertBuffDesc.VisibleNodeMask  = SharedVisibleMask;
        BufferData VBData{CubeVerts, sizeof(CubeVerts)};
        m_pDevice->CreateBuffer(VertBuffDesc, &VBData, &m_CubeVertexBuffer);
    }

    {
        BufferDesc IndBuffDesc;
        IndBuffDesc.Name             = "Cube index buffer";
        IndBuffDesc.Usage            = USAGE_IMMUTABLE;
        IndBuffDesc.BindFlags        = BIND_INDEX_BUFFER;
        IndBuffDesc.Size             = sizeof(CubeIndices);
        IndBuffDesc.CreationNodeMask = SharedCreationMask;
        IndBuffDesc.VisibleNodeMask  = SharedVisibleMask;
        BufferData IBData{CubeIndices, sizeof(CubeIndices)};
        m_pDevice->CreateBuffer(IndBuffDesc, &IBData, &m_CubeIndexBuffer);
    }

    for (Uint32 v = 0; v < m_NumViews; ++v)
    {
        const Uint32 NodeMask = m_bLinkedMultiGPU ? (1u << v) : 0u;

        BufferDesc CBDesc;
        CBDesc.Name             = "View constants CB";
        CBDesc.Size             = sizeof(CubeConstants);
        CBDesc.Usage            = USAGE_DYNAMIC;
        CBDesc.BindFlags        = BIND_UNIFORM_BUFFER;
        CBDesc.CPUAccessFlags   = CPU_ACCESS_WRITE;
        CBDesc.CreationNodeMask = NodeMask;
        CBDesc.VisibleNodeMask  = NodeMask;
        m_pDevice->CreateBuffer(CBDesc, nullptr, &m_ViewConstants[v]);
    }
}

void Tutorial31_LinkedMultiGPU::CreateCubePSO()
{
    GraphicsPipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = "Cube PSO";
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    PSOCreateInfo.GraphicsPipeline.NumRenderTargets             = 1;
    PSOCreateInfo.GraphicsPipeline.RTVFormats[0]                = m_ColorFmt;
    PSOCreateInfo.GraphicsPipeline.DSVFormat                    = m_DepthFmt;
    PSOCreateInfo.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    PSOCreateInfo.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_BACK;
    PSOCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = True;

    ShaderCreateInfo ShaderCI;
    ShaderCI.SourceLanguage                  = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.Desc.UseCombinedTextureSamplers = true;
    ShaderCI.CompileFlags                    = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;

    RefCntAutoPtr<IShader> pVS;
    {
        ShaderCI.Desc.ShaderType = SHADER_TYPE_VERTEX;
        ShaderCI.EntryPoint      = "main";
        ShaderCI.Desc.Name       = "Cube VS";
        ShaderCI.Source          = g_CubeVSSource;
        m_pDevice->CreateShader(ShaderCI, &pVS);
    }

    RefCntAutoPtr<IShader> pPS;
    {
        ShaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
        ShaderCI.EntryPoint      = "main";
        ShaderCI.Desc.Name       = "Cube PS";
        ShaderCI.Source          = g_CubePSSource;
        m_pDevice->CreateShader(ShaderCI, &pPS);
    }

    // clang-format off
    LayoutElement LayoutElems[] =
    {
        LayoutElement{0, 0, 3, VT_FLOAT32, False}, // Attribute 0 - position
        LayoutElement{1, 0, 4, VT_FLOAT32, False}  // Attribute 1 - color
    };
    // clang-format on
    PSOCreateInfo.GraphicsPipeline.InputLayout.LayoutElements = LayoutElems;
    PSOCreateInfo.GraphicsPipeline.InputLayout.NumElements    = _countof(LayoutElems);

    PSOCreateInfo.pVS = pVS;
    PSOCreateInfo.pPS = pPS;

    // The transformation constant buffer differs per view, so it is bound per SRB.
    ShaderResourceVariableDesc Vars[] = {
        {SHADER_TYPE_VERTEX, "Constants", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC} //
    };
    PSOCreateInfo.PSODesc.ResourceLayout.Variables    = Vars;
    PSOCreateInfo.PSODesc.ResourceLayout.NumVariables = _countof(Vars);

    m_pDevice->CreateGraphicsPipelineState(PSOCreateInfo, &m_pCubePSO);

    for (Uint32 v = 0; v < m_NumViews; ++v)
    {
        m_pCubePSO->CreateShaderResourceBinding(&m_CubeSRB[v], true);
        m_CubeSRB[v]->GetVariableByName(SHADER_TYPE_VERTEX, "Constants")->Set(m_ViewConstants[v]);
    }
}

void Tutorial31_LinkedMultiGPU::CreateCompositePSO()
{
    GraphicsPipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = "Composite PSO";
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    PSOCreateInfo.GraphicsPipeline.NumRenderTargets             = 1;
    PSOCreateInfo.GraphicsPipeline.RTVFormats[0]                = m_ColorFmt;
    PSOCreateInfo.GraphicsPipeline.DSVFormat                    = TEX_FORMAT_UNKNOWN;
    PSOCreateInfo.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    PSOCreateInfo.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    PSOCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    ShaderCreateInfo ShaderCI;
    ShaderCI.SourceLanguage                  = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.Desc.UseCombinedTextureSamplers = true;

    ShaderMacro Macros[] = {{"CONVERT_PS_OUTPUT_TO_GAMMA", m_ConvertPSOutputToGamma ? "1" : "0"}};
    ShaderCI.Macros      = {Macros, _countof(Macros)};

    RefCntAutoPtr<IShader> pVS;
    {
        ShaderCI.Desc.ShaderType = SHADER_TYPE_VERTEX;
        ShaderCI.EntryPoint      = "main";
        ShaderCI.Desc.Name       = "Composite VS";
        ShaderCI.Source          = g_CompositeVSSource;
        m_pDevice->CreateShader(ShaderCI, &pVS);
    }

    RefCntAutoPtr<IShader> pPS;
    {
        ShaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
        ShaderCI.EntryPoint      = "main";
        ShaderCI.Desc.Name       = "Composite PS";
        ShaderCI.Source          = g_CompositePSSource;
        m_pDevice->CreateShader(ShaderCI, &pPS);
    }

    PSOCreateInfo.pVS = pVS;
    PSOCreateInfo.pPS = pPS;

    ShaderResourceVariableDesc Vars[] = {
        {SHADER_TYPE_PIXEL, "g_Tex", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE} //
    };
    PSOCreateInfo.PSODesc.ResourceLayout.Variables    = Vars;
    PSOCreateInfo.PSODesc.ResourceLayout.NumVariables = _countof(Vars);

    SamplerDesc            SamLinearClamp{FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR,
                                          TEXTURE_ADDRESS_CLAMP, TEXTURE_ADDRESS_CLAMP, TEXTURE_ADDRESS_CLAMP};
    ImmutableSamplerDesc   ImtblSamplers[] = {
        {SHADER_TYPE_PIXEL, "g_Tex", SamLinearClamp} //
    };
    PSOCreateInfo.PSODesc.ResourceLayout.ImmutableSamplers    = ImtblSamplers;
    PSOCreateInfo.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(ImtblSamplers);

    m_pDevice->CreateGraphicsPipelineState(PSOCreateInfo, &m_pCompositePSO);
}

void Tutorial31_LinkedMultiGPU::CreateViewRenderTargets(Uint32 Width, Uint32 Height)
{
    if (Width == 0 || Height == 0 || m_NumViews == 0)
        return;

    m_RTWidth  = std::max(1u, Width / m_NumViews);
    m_RTHeight = std::max(1u, Height);

    for (Uint32 v = 0; v < m_NumViews; ++v)
    {
        const Uint32 OwnerNodeMask   = m_bLinkedMultiGPU ? (1u << v) : 0u;
        // The color target must be readable by node 0 (which composes the final image).
        const Uint32 VisibleNodeMask = m_bLinkedMultiGPU ? ((1u << v) | 1u) : 0u;

        m_ColorRTV[v].Release();
        m_ColorSRV[v].Release();
        m_DepthDSV[v].Release();
        m_CompositeSRB[v].Release();

        TextureDesc ColorDesc;
        ColorDesc.Name             = "View color target";
        ColorDesc.Type             = RESOURCE_DIM_TEX_2D;
        ColorDesc.Width            = m_RTWidth;
        ColorDesc.Height           = m_RTHeight;
        ColorDesc.MipLevels        = 1;
        ColorDesc.Format           = m_ColorFmt;
        ColorDesc.BindFlags        = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
        ColorDesc.ClearValue.Format   = m_ColorFmt;
        ColorDesc.ClearValue.Color[0] = 0.05f;
        ColorDesc.ClearValue.Color[1] = 0.05f;
        ColorDesc.ClearValue.Color[2] = 0.08f;
        ColorDesc.ClearValue.Color[3] = 1.0f;
        ColorDesc.CreationNodeMask = OwnerNodeMask;
        ColorDesc.VisibleNodeMask  = VisibleNodeMask;

        RefCntAutoPtr<ITexture> pColor;
        m_pDevice->CreateTexture(ColorDesc, nullptr, &pColor);
        m_ColorRTV[v] = pColor->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        m_ColorSRV[v] = pColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

        TextureDesc DepthDesc;
        DepthDesc.Name             = "View depth target";
        DepthDesc.Type             = RESOURCE_DIM_TEX_2D;
        DepthDesc.Width            = m_RTWidth;
        DepthDesc.Height           = m_RTHeight;
        DepthDesc.MipLevels        = 1;
        DepthDesc.Format           = m_DepthFmt;
        DepthDesc.BindFlags        = BIND_DEPTH_STENCIL;
        DepthDesc.ClearValue.Format               = m_DepthFmt;
        DepthDesc.ClearValue.DepthStencil.Depth   = 1.0f;
        DepthDesc.ClearValue.DepthStencil.Stencil = 0;
        DepthDesc.CreationNodeMask = OwnerNodeMask;
        DepthDesc.VisibleNodeMask  = OwnerNodeMask;

        RefCntAutoPtr<ITexture> pDepth;
        m_pDevice->CreateTexture(DepthDesc, nullptr, &pDepth);
        m_DepthDSV[v] = pDepth->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);

        m_pCompositePSO->CreateShaderResourceBinding(&m_CompositeSRB[v], true);
        m_CompositeSRB[v]->GetVariableByName(SHADER_TYPE_PIXEL, "g_Tex")->Set(m_ColorSRV[v]);
    }
}

void Tutorial31_LinkedMultiGPU::Update(double CurrTime, double ElapsedTime, bool DoUpdateUI)
{
    SampleBase::Update(CurrTime, ElapsedTime, DoUpdateUI);

    const float4x4 Model = float4x4::RotationY(static_cast<float>(CurrTime)) * float4x4::RotationX(-PI_F * 0.1f);
    const float4x4 View  = float4x4::Translation(0.f, 0.f, 5.f);

    const bool  IsGL   = m_pDevice->GetDeviceInfo().IsGLDevice();
    const float Aspect = static_cast<float>(m_RTWidth) / static_cast<float>(std::max(1u, m_RTHeight));
    const float4x4 Proj = float4x4::Projection(PI_F / 4.f, Aspect, 0.1f, 100.f, IsGL);

    for (Uint32 v = 0; v < m_NumViews; ++v)
        m_ViewWorldViewProj[v] = Model * View * Proj;

    m_GpuInfoPanel.Update(ElapsedTime);
}

void Tutorial31_LinkedMultiGPU::UpdateUI()
{
    m_GpuInfoPanel.UpdateUI("GPU Info");
}

void Tutorial31_LinkedMultiGPU::RenderViewScene(IDeviceContext* pCtx, Uint32 ViewId)
{
    {
        MapHelper<CubeConstants> CBData(pCtx, m_ViewConstants[ViewId], MAP_WRITE, MAP_FLAG_DISCARD);
        CBData->WorldViewProj = m_ViewWorldViewProj[ViewId];
        // Give each node a distinct tint so it is visible which node produced which strip.
        CBData->Tint = (ViewId == 0) ? float4{1.0f, 0.78f, 0.78f, 1.f} : float4{0.78f, 0.85f, 1.0f, 1.f};
    }

    ITextureView* pRTV = m_ColorRTV[ViewId];
    ITextureView* pDSV = m_DepthDSV[ViewId];
    pCtx->SetRenderTargets(1, &pRTV, pDSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    const float ClearColor[] = {0.05f, 0.05f, 0.08f, 1.0f};
    pCtx->ClearRenderTarget(pRTV, ClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pCtx->ClearDepthStencil(pDSV, CLEAR_DEPTH_FLAG, 1.f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    const Uint64 Offset   = 0;
    IBuffer*     pBuffs[] = {m_CubeVertexBuffer};
    pCtx->SetVertexBuffers(0, 1, pBuffs, &Offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
    pCtx->SetIndexBuffer(m_CubeIndexBuffer, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    pCtx->SetPipelineState(m_pCubePSO);
    pCtx->CommitShaderResources(m_CubeSRB[ViewId], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DrawIndexedAttribs DrawAttrs;
    DrawAttrs.IndexType  = VT_UINT32;
    DrawAttrs.NumIndices = 36;
    DrawAttrs.Flags      = DRAW_FLAG_VERIFY_ALL;
    pCtx->DrawIndexed(DrawAttrs);
}

void Tutorial31_LinkedMultiGPU::Render()
{
    if (m_NumViews == 0)
        return;

    // 1) Render each view's scene on its own node's immediate context.
    for (Uint32 v = 0; v < m_NumViews; ++v)
    {
        RenderViewScene(m_ViewContexts[v], v);

        if (m_bLinkedMultiGPU)
        {
            // Submit this node's work and signal a fence so node 0 can wait for it.
            m_ViewContexts[v]->EnqueueSignal(m_ViewFence[v], m_FenceValue + 1);
            m_ViewContexts[v]->Flush();
        }
    }
    if (m_bLinkedMultiGPU)
        ++m_FenceValue;

    // 2) Compose all strips into the swap chain back buffer on the primary (node 0) context.
    IDeviceContext* pCtx = m_ViewContexts[0];

    if (m_bLinkedMultiGPU)
    {
        for (Uint32 v = 1; v < m_NumViews; ++v)
            pCtx->DeviceWaitForFence(m_ViewFence[v], m_FenceValue);
    }

    ITextureView* pRTV = m_pSwapChain->GetCurrentBackBufferRTV();
    pCtx->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // The final background is black, which is identical in linear and gamma (sRGB) space.
    const float ClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    pCtx->ClearRenderTarget(pRTV, ClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    pCtx->SetPipelineState(m_pCompositePSO);

    const SwapChainDesc& SCDesc = m_pSwapChain->GetDesc();
    for (Uint32 v = 0; v < m_NumViews; ++v)
    {
        Viewport VP;
        VP.TopLeftX = static_cast<float>(v * m_RTWidth);
        VP.TopLeftY = 0.f;
        VP.Width    = static_cast<float>(m_RTWidth);
        VP.Height   = static_cast<float>(SCDesc.Height);
        VP.MinDepth = 0.f;
        VP.MaxDepth = 1.f;
        pCtx->SetViewports(1, &VP, SCDesc.Width, SCDesc.Height);

        pCtx->CommitShaderResources(m_CompositeSRB[v], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        DrawAttribs DrawAttrs;
        DrawAttrs.NumVertices = 3;
        DrawAttrs.Flags       = DRAW_FLAG_VERIFY_ALL;
        pCtx->Draw(DrawAttrs);
    }
}

void Tutorial31_LinkedMultiGPU::WindowResize(Uint32 Width, Uint32 Height)
{
    if (!m_pCompositePSO)
        return;

    // Make sure no GPU work is referencing the old targets.
    for (Uint32 v = 0; v < m_NumViews; ++v)
    {
        if (m_ViewContexts[v])
            m_ViewContexts[v]->WaitForIdle();
    }

    CreateViewRenderTargets(Width, Height);
}

} // namespace Diligent
