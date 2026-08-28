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

#pragma once

#include <vector>

#include "SampleBase.hpp"
#include "BasicMath.hpp"
#include "../../Common/src/GpuInfoPanel.hpp"

namespace Diligent
{

/// Tutorial demonstrating Linked Multi-GPU (LDA / Device Group) split-frame rendering.
///
/// A single logical IRenderDevice manages multiple physical GPU nodes. The screen is split
/// into vertical strips: each GPU node renders one strip into its own render target (allocated
/// on that node), and node 0 then composes all strips into the swap chain back buffer.
///
/// On systems with a single GPU node (the common case), the sample gracefully falls back to a
/// single view and renders the whole scene on one node. Multi-node distribution is exercised
/// automatically when the D3D12/Vulkan device reports more than one linked node.
class Tutorial31_LinkedMultiGPU final : public SampleBase
{
public:
    virtual void ModifyEngineInitInfo(const ModifyEngineInitInfoAttribs& Attribs) override final;
    virtual void Initialize(const SampleInitInfo& InitInfo) override final;

    virtual void Render() override final;
    virtual void Update(double CurrTime, double ElapsedTime, bool DoUpdateUI) override final;

    virtual void WindowResize(Uint32 Width, Uint32 Height) override final;

    virtual const Char* GetSampleName() const override final { return "Tutorial31: Linked Multi-GPU"; }

private:
    void CreateCubePSO();
    void CreateCompositePSO();
    void CreateCubeBuffers();
    void CreateViewRenderTargets(Uint32 Width, Uint32 Height);
    void RenderViewScene(IDeviceContext* pCtx, Uint32 ViewId);

    virtual void UpdateUI() override final;

    // Uniform block shared with the cube shader (must match HLSL layout).
    struct CubeConstants
    {
        float4x4 WorldViewProj;
        float4   Tint;
    };

    // Maximum number of GPU nodes/views the sample will drive.
    static constexpr Uint32 MaxViews = 2;

    Uint32 m_NumViews        = 1;
    bool   m_bLinkedMultiGPU = false;
    Uint32 m_AdapterNodeMask = 1;

    // Immediate-context descriptors requested in ModifyEngineInitInfo (one per node).
    // Must stay alive until the device is created, hence it is a member.
    std::vector<ImmediateContextCreateInfo> m_ContextCI;

    // One immediate context per view/node. m_ViewContexts[0] is the primary (present) context.
    RefCntAutoPtr<IDeviceContext> m_ViewContexts[MaxViews];

    // Scene (cube) resources.
    RefCntAutoPtr<IPipelineState>         m_pCubePSO;
    RefCntAutoPtr<IBuffer>                m_CubeVertexBuffer;
    RefCntAutoPtr<IBuffer>                m_CubeIndexBuffer;
    RefCntAutoPtr<IBuffer>                m_ViewConstants[MaxViews];
    RefCntAutoPtr<IShaderResourceBinding> m_CubeSRB[MaxViews];

    // Composition (blit) resources.
    RefCntAutoPtr<IPipelineState>         m_pCompositePSO;
    RefCntAutoPtr<IShaderResourceBinding> m_CompositeSRB[MaxViews];

    // Per-view off-screen render targets (color + depth).
    RefCntAutoPtr<ITextureView> m_ColorRTV[MaxViews];
    RefCntAutoPtr<ITextureView> m_ColorSRV[MaxViews];
    RefCntAutoPtr<ITextureView> m_DepthDSV[MaxViews];

    // Cross-node synchronization: node 0 waits for the other nodes before composing.
    RefCntAutoPtr<IFence> m_ViewFence[MaxViews];
    Uint64                m_FenceValue = 0;

    float4x4       m_ViewWorldViewProj[MaxViews];
    TEXTURE_FORMAT m_ColorFmt  = TEX_FORMAT_RGBA8_UNORM_SRGB;
    TEXTURE_FORMAT m_DepthFmt  = TEX_FORMAT_D32_FLOAT;
    Uint32         m_RTWidth   = 0;
    Uint32         m_RTHeight  = 0;

    GpuInfoPanel m_GpuInfoPanel;
};

} // namespace Diligent
