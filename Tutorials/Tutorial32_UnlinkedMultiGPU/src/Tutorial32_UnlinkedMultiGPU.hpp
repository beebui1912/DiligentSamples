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

#include "SampleBase.hpp"
#include "BasicMath.hpp"
#include "CrossDeviceTransferManager.hpp"
#include "../../Common/src/GpuInfoPanel.hpp"

namespace Diligent
{

/// Tutorial demonstrating Unlinked Multi-GPU rendering across two independent devices.
///
/// Unlike linked mode, the two GPUs (or two independent devices on the same GPU) do not share
/// memory, so results must be copied from the secondary device to the primary device through a
/// host-visible staging path. This is handled by Diligent's CrossDeviceTransferManager.
///
/// - The primary device (created by the framework) owns the swap chain and renders the left half.
/// - A secondary device (created by this sample) renders the right half into its own render target.
/// - The secondary result is transferred GPU -> CPU -> GPU into a primary-device texture.
/// - The primary device composes both halves into the swap chain back buffer.
///
/// On systems where a second device cannot be created, the sample falls back to a single view.
class Tutorial32_UnlinkedMultiGPU final : public SampleBase
{
public:
    ~Tutorial32_UnlinkedMultiGPU() override;

    virtual void Initialize(const SampleInitInfo& InitInfo) override final;

    virtual void Render() override final;
    virtual void Update(double CurrTime, double ElapsedTime, bool DoUpdateUI) override final;

    virtual void WindowResize(Uint32 Width, Uint32 Height) override final;

    virtual const Char* GetSampleName() const override final { return "Tutorial32: Unlinked Multi-GPU"; }

private:
    // Uniform block shared with the cube shader (must match HLSL layout).
    struct CubeConstants
    {
        float4x4 WorldViewProj;
        float4   Tint;
    };

    // Everything needed to render the scene on one device. Resources are device-specific in
    // unlinked mode, so each device owns its own copy.
    struct SceneResources
    {
        RefCntAutoPtr<IPipelineState>         pPSO;
        RefCntAutoPtr<IBuffer>                pVertexBuffer;
        RefCntAutoPtr<IBuffer>                pIndexBuffer;
        RefCntAutoPtr<IBuffer>                pConstants;
        RefCntAutoPtr<IShaderResourceBinding> pSRB;

        RefCntAutoPtr<ITexture>     pColor;
        RefCntAutoPtr<ITextureView> pColorRTV;
        RefCntAutoPtr<ITextureView> pColorSRV;
        RefCntAutoPtr<ITextureView> pDepthDSV;
    };

    bool CreateSecondaryDevice();
    void CreateSceneResources(IRenderDevice* pDevice, SceneResources& Res);
    void CreateSceneRenderTarget(IRenderDevice* pDevice, SceneResources& Res);
    void CreateCompositePSO();
    void CreateCompositeResources();
    void CreateAllRenderTargets(Uint32 Width, Uint32 Height);
    void RenderScene(IDeviceContext* pCtx, SceneResources& Res, const float4& Tint);

    virtual void UpdateUI() override final;

    // Secondary device/context are declared first so they are released last (after the resources
    // that live on them).
    RefCntAutoPtr<IRenderDevice>  m_pSecondaryDevice;
    RefCntAutoPtr<IDeviceContext> m_pSecondaryContext;

    SceneResources m_Primary;   // renders the left half on the primary device
    SceneResources m_Secondary; // renders the right half on the secondary device

    // Composition (primary device only).
    RefCntAutoPtr<IPipelineState>         m_pCompositePSO;
    RefCntAutoPtr<IShaderResourceBinding> m_CompositeSRB[2];

    // Primary-device texture that receives the transferred secondary result.
    RefCntAutoPtr<ITexture>     m_pReceivedTexture;
    RefCntAutoPtr<ITextureView> m_pReceivedSRV;

    // Manages the GPU -> CPU -> GPU transfer of the secondary result to the primary device.
    CrossDeviceTransferManager m_TransferMgr;

    Uint32 m_NumViews     = 1;
    bool   m_bMultiDevice = false;
    Uint32 m_FrameId      = 0;

    float4x4       m_WorldViewProj;
    TEXTURE_FORMAT m_ColorFmt  = TEX_FORMAT_RGBA8_UNORM_SRGB;
    TEXTURE_FORMAT m_DepthFmt  = TEX_FORMAT_D32_FLOAT;
    Uint32         m_RTWidth   = 0;
    Uint32         m_RTHeight  = 0;

    GpuInfoPanel m_GpuInfoPanel;
};

} // namespace Diligent
