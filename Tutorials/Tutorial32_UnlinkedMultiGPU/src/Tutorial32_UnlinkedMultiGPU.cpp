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
#include <cstring>
#include <vector>

#include "Tutorial32_UnlinkedMultiGPU.hpp"
#include "MapHelper.hpp"
#include "GraphicsAccessories.hpp"
#include "ColorConversion.h"

#if D3D12_SUPPORTED
#    include "EngineFactoryD3D12.h"
#endif
#if VULKAN_SUPPORTED
#    include "EngineFactoryVk.h"
#endif

namespace Diligent
{

SampleBase* CreateSample()
{
    return new Tutorial32_UnlinkedMultiGPU();
}

namespace
{

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

// Selects an adapter for the secondary device: a different adapter than the primary if the system
// has more than one, otherwise the default adapter (the same physical GPU, which still demonstrates
// the unlinked cross-device transfer path).
template <typename FactoryType>
Uint32 SelectSecondaryAdapter(FactoryType* pFactory, const Version& APIVersion, IRenderDevice* pPrimaryDevice)
{
    Uint32 NumAdapters = 0;
    pFactory->EnumerateAdapters(APIVersion, NumAdapters, nullptr);
    if (NumAdapters <= 1)
        return DEFAULT_ADAPTER_ID;

    std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
    pFactory->EnumerateAdapters(APIVersion, NumAdapters, Adapters.data());

    const char* PrimaryDesc = pPrimaryDevice->GetAdapterInfo().Description;
    for (Uint32 i = 0; i < NumAdapters; ++i)
    {
        if (std::strcmp(Adapters[i].Description, PrimaryDesc) != 0)
            return i;
    }
    return DEFAULT_ADAPTER_ID;
}

} // namespace

Tutorial32_UnlinkedMultiGPU::~Tutorial32_UnlinkedMultiGPU()
{
    // Make sure no GPU work is in flight before resources are released.
    if (m_pImmediateContext)
        m_pImmediateContext->WaitForIdle();
    if (m_pSecondaryContext)
        m_pSecondaryContext->WaitForIdle();

    m_TransferMgr.Reset();
}

bool Tutorial32_UnlinkedMultiGPU::CreateSecondaryDevice()
{
    const RENDER_DEVICE_TYPE Type = m_pDevice->GetDeviceInfo().Type;

#if D3D12_SUPPORTED
    if (Type == RENDER_DEVICE_TYPE_D3D12)
    {
        IEngineFactoryD3D12* pRawFactory = nullptr;
        m_pEngineFactory->QueryInterface(IID_EngineFactoryD3D12, reinterpret_cast<IObject**>(&pRawFactory));
        RefCntAutoPtr<IEngineFactoryD3D12> pFactoryD3D12;
        pFactoryD3D12.Attach(pRawFactory);
        if (!pFactoryD3D12)
            return false;

        EngineD3D12CreateInfo EngineCI;
        EngineCI.GraphicsAPIVersion = Version{11, 0};
        EngineCI.AdapterId          = SelectSecondaryAdapter(pFactoryD3D12.RawPtr(), EngineCI.GraphicsAPIVersion, m_pDevice);

        IDeviceContext* ppContexts[1] = {};
        pFactoryD3D12->CreateDeviceAndContextsD3D12(EngineCI, &m_pSecondaryDevice, ppContexts);
        if (!m_pSecondaryDevice)
            return false;
        m_pSecondaryContext.Attach(ppContexts[0]);
        return m_pSecondaryContext != nullptr;
    }
#endif

#if VULKAN_SUPPORTED
    if (Type == RENDER_DEVICE_TYPE_VULKAN)
    {
        IEngineFactoryVk* pRawFactory = nullptr;
        m_pEngineFactory->QueryInterface(IID_EngineFactoryVk, reinterpret_cast<IObject**>(&pRawFactory));
        RefCntAutoPtr<IEngineFactoryVk> pFactoryVk;
        pFactoryVk.Attach(pRawFactory);
        if (!pFactoryVk)
            return false;

        EngineVkCreateInfo EngineCI;
        EngineCI.AdapterId = SelectSecondaryAdapter(pFactoryVk.RawPtr(), EngineCI.GraphicsAPIVersion, m_pDevice);

        IDeviceContext* ppContexts[1] = {};
        pFactoryVk->CreateDeviceAndContextsVk(EngineCI, &m_pSecondaryDevice, ppContexts);
        if (!m_pSecondaryDevice)
            return false;
        m_pSecondaryContext.Attach(ppContexts[0]);
        return m_pSecondaryContext != nullptr;
    }
#endif

    // Unlinked multi-GPU with independent devices is only wired up for D3D12 and Vulkan here.
    return false;
}

void Tutorial32_UnlinkedMultiGPU::CreateSceneResources(IRenderDevice* pDevice, SceneResources& Res)
{
    {
        BufferDesc VertBuffDesc;
        VertBuffDesc.Name      = "Cube vertex buffer";
        VertBuffDesc.Usage     = USAGE_IMMUTABLE;
        VertBuffDesc.BindFlags = BIND_VERTEX_BUFFER;
        VertBuffDesc.Size      = sizeof(CubeVerts);
        BufferData VBData{CubeVerts, sizeof(CubeVerts)};
        pDevice->CreateBuffer(VertBuffDesc, &VBData, &Res.pVertexBuffer);
    }
    {
        BufferDesc IndBuffDesc;
        IndBuffDesc.Name      = "Cube index buffer";
        IndBuffDesc.Usage     = USAGE_IMMUTABLE;
        IndBuffDesc.BindFlags = BIND_INDEX_BUFFER;
        IndBuffDesc.Size      = sizeof(CubeIndices);
        BufferData IBData{CubeIndices, sizeof(CubeIndices)};
        pDevice->CreateBuffer(IndBuffDesc, &IBData, &Res.pIndexBuffer);
    }
    {
        BufferDesc CBDesc;
        CBDesc.Name           = "View constants CB";
        CBDesc.Size           = sizeof(CubeConstants);
        CBDesc.Usage          = USAGE_DYNAMIC;
        CBDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        CBDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
        pDevice->CreateBuffer(CBDesc, nullptr, &Res.pConstants);
    }

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
        pDevice->CreateShader(ShaderCI, &pVS);
    }
    RefCntAutoPtr<IShader> pPS;
    {
        ShaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
        ShaderCI.EntryPoint      = "main";
        ShaderCI.Desc.Name       = "Cube PS";
        ShaderCI.Source          = g_CubePSSource;
        pDevice->CreateShader(ShaderCI, &pPS);
    }

    // clang-format off
    LayoutElement LayoutElems[] =
    {
        LayoutElement{0, 0, 3, VT_FLOAT32, False},
        LayoutElement{1, 0, 4, VT_FLOAT32, False}
    };
    // clang-format on
    PSOCreateInfo.GraphicsPipeline.InputLayout.LayoutElements = LayoutElems;
    PSOCreateInfo.GraphicsPipeline.InputLayout.NumElements    = _countof(LayoutElems);

    PSOCreateInfo.pVS = pVS;
    PSOCreateInfo.pPS = pPS;

    ShaderResourceVariableDesc Vars[] = {
        {SHADER_TYPE_VERTEX, "Constants", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC} //
    };
    PSOCreateInfo.PSODesc.ResourceLayout.Variables    = Vars;
    PSOCreateInfo.PSODesc.ResourceLayout.NumVariables = _countof(Vars);

    pDevice->CreateGraphicsPipelineState(PSOCreateInfo, &Res.pPSO);
    Res.pPSO->CreateShaderResourceBinding(&Res.pSRB, true);
    Res.pSRB->GetVariableByName(SHADER_TYPE_VERTEX, "Constants")->Set(Res.pConstants);
}

void Tutorial32_UnlinkedMultiGPU::CreateSceneRenderTarget(IRenderDevice* pDevice, SceneResources& Res)
{
    Res.pColor.Release();
    Res.pColorRTV.Release();
    Res.pColorSRV.Release();
    Res.pDepthDSV.Release();

    TextureDesc ColorDesc;
    ColorDesc.Name                = "Scene color target";
    ColorDesc.Type                = RESOURCE_DIM_TEX_2D;
    ColorDesc.Width               = m_RTWidth;
    ColorDesc.Height              = m_RTHeight;
    ColorDesc.MipLevels           = 1;
    ColorDesc.Format              = m_ColorFmt;
    ColorDesc.BindFlags           = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    ColorDesc.ClearValue.Format   = m_ColorFmt;
    ColorDesc.ClearValue.Color[0] = 0.05f;
    ColorDesc.ClearValue.Color[1] = 0.05f;
    ColorDesc.ClearValue.Color[2] = 0.08f;
    ColorDesc.ClearValue.Color[3] = 1.0f;
    pDevice->CreateTexture(ColorDesc, nullptr, &Res.pColor);
    Res.pColorRTV = Res.pColor->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    Res.pColorSRV = Res.pColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

    TextureDesc DepthDesc;
    DepthDesc.Name                          = "Scene depth target";
    DepthDesc.Type                          = RESOURCE_DIM_TEX_2D;
    DepthDesc.Width                         = m_RTWidth;
    DepthDesc.Height                        = m_RTHeight;
    DepthDesc.MipLevels                     = 1;
    DepthDesc.Format                        = m_DepthFmt;
    DepthDesc.BindFlags                     = BIND_DEPTH_STENCIL;
    DepthDesc.ClearValue.Format             = m_DepthFmt;
    DepthDesc.ClearValue.DepthStencil.Depth = 1.0f;
    RefCntAutoPtr<ITexture> pDepth;
    pDevice->CreateTexture(DepthDesc, nullptr, &pDepth);
    Res.pDepthDSV = pDepth->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
}

void Tutorial32_UnlinkedMultiGPU::CreateCompositePSO()
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

    SamplerDesc          SamLinearClamp{FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR,
                                        TEXTURE_ADDRESS_CLAMP, TEXTURE_ADDRESS_CLAMP, TEXTURE_ADDRESS_CLAMP};
    ImmutableSamplerDesc ImtblSamplers[] = {
        {SHADER_TYPE_PIXEL, "g_Tex", SamLinearClamp} //
    };
    PSOCreateInfo.PSODesc.ResourceLayout.ImmutableSamplers    = ImtblSamplers;
    PSOCreateInfo.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(ImtblSamplers);

    m_pDevice->CreateGraphicsPipelineState(PSOCreateInfo, &m_pCompositePSO);
}

void Tutorial32_UnlinkedMultiGPU::CreateAllRenderTargets(Uint32 Width, Uint32 Height)
{
    if (Width == 0 || Height == 0)
        return;

    // Parenthesize std::max to avoid collision with the Windows max() macro (windows.h is pulled
    // in by the engine factory headers included above).
    m_RTWidth  = (std::max)(1u, Width / m_NumViews);
    m_RTHeight = (std::max)(1u, Height);

    CreateSceneRenderTarget(m_pDevice, m_Primary);

    m_CompositeSRB[0].Release();
    m_CompositeSRB[1].Release();
    m_pReceivedTexture.Release();
    m_pReceivedSRV.Release();

    if (m_bMultiDevice)
    {
        CreateSceneRenderTarget(m_pSecondaryDevice, m_Secondary);

        // Primary-device texture that receives the transferred secondary result.
        TextureDesc RecvDesc;
        RecvDesc.Name                = "Received (secondary) texture";
        RecvDesc.Type                = RESOURCE_DIM_TEX_2D;
        RecvDesc.Width               = m_RTWidth;
        RecvDesc.Height              = m_RTHeight;
        RecvDesc.MipLevels           = 1;
        RecvDesc.Format              = m_ColorFmt;
        RecvDesc.BindFlags           = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
        RecvDesc.ClearValue.Format   = m_ColorFmt;
        RecvDesc.ClearValue.Color[0] = 0.05f;
        RecvDesc.ClearValue.Color[1] = 0.05f;
        RecvDesc.ClearValue.Color[2] = 0.08f;
        RecvDesc.ClearValue.Color[3] = 1.0f;
        m_pDevice->CreateTexture(RecvDesc, nullptr, &m_pReceivedTexture);
        m_pReceivedSRV = m_pReceivedTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

        // Clear it once so the first frames (before the transfer pipeline fills up) are not garbage.
        ITextureView* pRecvRTV = m_pReceivedTexture->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        const float   ClearColor[] = {0.05f, 0.05f, 0.08f, 1.0f};
        m_pImmediateContext->SetRenderTargets(1, &pRecvRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_pImmediateContext->ClearRenderTarget(pRecvRTV, ClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // (Re)initialize the cross-device transfer manager for the current target size.
        TextureDesc TransferDesc;
        TransferDesc.Type   = RESOURCE_DIM_TEX_2D;
        TransferDesc.Width  = m_RTWidth;
        TransferDesc.Height = m_RTHeight;
        TransferDesc.Format = m_ColorFmt;
        m_TransferMgr.Init(m_pSecondaryDevice, m_pDevice, TransferDesc, 2);
        m_FrameId = 0;
    }

    // Composite bindings: strip 0 samples the primary render target, strip 1 samples the received texture.
    m_pCompositePSO->CreateShaderResourceBinding(&m_CompositeSRB[0], true);
    m_CompositeSRB[0]->GetVariableByName(SHADER_TYPE_PIXEL, "g_Tex")->Set(m_Primary.pColorSRV);

    if (m_bMultiDevice)
    {
        m_pCompositePSO->CreateShaderResourceBinding(&m_CompositeSRB[1], true);
        m_CompositeSRB[1]->GetVariableByName(SHADER_TYPE_PIXEL, "g_Tex")->Set(m_pReceivedSRV);
    }
}

void Tutorial32_UnlinkedMultiGPU::Initialize(const SampleInitInfo& InitInfo)
{
    SampleBase::Initialize(InitInfo);

    const SwapChainDesc& SCDesc = m_pSwapChain->GetDesc();
    m_ColorFmt                  = SCDesc.ColorBufferFormat;
    m_DepthFmt                  = SCDesc.DepthBufferFormat != TEX_FORMAT_UNKNOWN ? SCDesc.DepthBufferFormat : TEX_FORMAT_D32_FLOAT;

    m_bMultiDevice = CreateSecondaryDevice();
    m_NumViews     = m_bMultiDevice ? 2 : 1;

    if (m_bMultiDevice)
    {
        LOG_INFO_MESSAGE("Tutorial32: created a secondary device on '", m_pSecondaryDevice->GetAdapterInfo().Description,
                         "'. Rendering the right half on the secondary device and transferring the result to the primary device.");
    }
    else
    {
        LOG_INFO_MESSAGE("Tutorial32: could not create a secondary device (backend may not support it). "
                         "Running in single-device mode.");
    }

    CreateSceneResources(m_pDevice, m_Primary);
    if (m_bMultiDevice)
        CreateSceneResources(m_pSecondaryDevice, m_Secondary);

    CreateCompositePSO();
    CreateAllRenderTargets(SCDesc.Width, SCDesc.Height);

    IRenderDevice* AppDevices[2] = {m_pDevice, m_pSecondaryDevice};
    m_GpuInfoPanel.Initialize(m_pEngineFactory, AppDevices, m_bMultiDevice ? 2u : 1u);
}

void Tutorial32_UnlinkedMultiGPU::Update(double CurrTime, double ElapsedTime, bool DoUpdateUI)
{
    SampleBase::Update(CurrTime, ElapsedTime, DoUpdateUI);

    const float4x4 Model = float4x4::RotationY(static_cast<float>(CurrTime)) * float4x4::RotationX(-PI_F * 0.1f);
    const float4x4 View  = float4x4::Translation(0.f, 0.f, 5.f);

    const bool     IsGL   = m_pDevice->GetDeviceInfo().IsGLDevice();
    const float    Aspect = static_cast<float>(m_RTWidth) / static_cast<float>((std::max)(1u, m_RTHeight));
    const float4x4 Proj   = float4x4::Projection(PI_F / 4.f, Aspect, 0.1f, 100.f, IsGL);

    m_WorldViewProj = Model * View * Proj;

    m_GpuInfoPanel.Update(ElapsedTime);
}

void Tutorial32_UnlinkedMultiGPU::UpdateUI()
{
    m_GpuInfoPanel.UpdateUI("GPU Info");
}

void Tutorial32_UnlinkedMultiGPU::RenderScene(IDeviceContext* pCtx, SceneResources& Res, const float4& Tint)
{
    {
        MapHelper<CubeConstants> CBData(pCtx, Res.pConstants, MAP_WRITE, MAP_FLAG_DISCARD);
        CBData->WorldViewProj = m_WorldViewProj;
        CBData->Tint          = Tint;
    }

    ITextureView* pRTV = Res.pColorRTV;
    ITextureView* pDSV = Res.pDepthDSV;
    pCtx->SetRenderTargets(1, &pRTV, pDSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    const float ClearColor[] = {0.05f, 0.05f, 0.08f, 1.0f};
    pCtx->ClearRenderTarget(pRTV, ClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pCtx->ClearDepthStencil(pDSV, CLEAR_DEPTH_FLAG, 1.f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    const Uint64 Offset   = 0;
    IBuffer*     pBuffs[] = {Res.pVertexBuffer};
    pCtx->SetVertexBuffers(0, 1, pBuffs, &Offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
    pCtx->SetIndexBuffer(Res.pIndexBuffer, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    pCtx->SetPipelineState(Res.pPSO);
    pCtx->CommitShaderResources(Res.pSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DrawIndexedAttribs DrawAttrs;
    DrawAttrs.IndexType  = VT_UINT32;
    DrawAttrs.NumIndices = 36;
    DrawAttrs.Flags      = DRAW_FLAG_VERIFY_ALL;
    pCtx->DrawIndexed(DrawAttrs);
}

void Tutorial32_UnlinkedMultiGPU::Render()
{
    // Left half on the primary device.
    RenderScene(m_pImmediateContext, m_Primary, float4{1.0f, 0.78f, 0.78f, 1.f});

    if (m_bMultiDevice)
    {
        // Right half on the secondary device.
        RenderScene(m_pSecondaryContext, m_Secondary, float4{0.78f, 0.85f, 1.0f, 1.f});

        // Copy the secondary result into its host-visible staging texture and submit the work.
        m_TransferMgr.CopySourceFrame(m_pSecondaryContext, m_Secondary.pColor, m_FrameId);
        m_pSecondaryContext->Flush();

        // Bring a ready frame across to the primary device's texture (GPU -> CPU -> GPU).
        m_TransferMgr.TransferReadyFrame(m_pSecondaryContext, m_pImmediateContext, m_pReceivedTexture, m_FrameId);
    }

    // Compose the halves into the swap chain back buffer on the primary device.
    ITextureView* pRTV = m_pSwapChain->GetCurrentBackBufferRTV();
    m_pImmediateContext->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // The final background is black, which is identical in linear and gamma (sRGB) space.
    const float ClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_pImmediateContext->ClearRenderTarget(pRTV, ClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    m_pImmediateContext->SetPipelineState(m_pCompositePSO);

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
        m_pImmediateContext->SetViewports(1, &VP, SCDesc.Width, SCDesc.Height);

        m_pImmediateContext->CommitShaderResources(m_CompositeSRB[v], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        DrawAttribs DrawAttrs;
        DrawAttrs.NumVertices = 3;
        DrawAttrs.Flags       = DRAW_FLAG_VERIFY_ALL;
        m_pImmediateContext->Draw(DrawAttrs);
    }

    ++m_FrameId;
}

void Tutorial32_UnlinkedMultiGPU::WindowResize(Uint32 Width, Uint32 Height)
{
    if (!m_pCompositePSO)
        return;

    if (m_pImmediateContext)
        m_pImmediateContext->WaitForIdle();
    if (m_pSecondaryContext)
        m_pSecondaryContext->WaitForIdle();

    CreateAllRenderTargets(Width, Height);
}

} // namespace Diligent
