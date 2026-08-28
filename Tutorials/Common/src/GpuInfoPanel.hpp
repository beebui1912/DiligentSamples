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

#include <memory>
#include <string>
#include <vector>

#include "RenderDevice.h"
#include "EngineFactory.h"

namespace Diligent
{

/// Small helper that renders an ImGui panel listing the GPUs in the system together with their
/// video-memory usage and the application's GPU utilization.
///
/// Live statistics are gathered on Windows:
///   - VRAM used / budget via DXGI `IDXGIAdapter3::QueryVideoMemoryInfo` (per process).
///   - GPU utilization (%) via the PDH "GPU Engine" performance counters, summed for this process.
///
/// On other platforms (or if the queries are unavailable) the panel falls back to static adapter
/// information (name, type and total VRAM) reported by the engine.
class GpuInfoPanel
{
public:
    GpuInfoPanel();
    ~GpuInfoPanel();

    GpuInfoPanel(const GpuInfoPanel&)            = delete;
    GpuInfoPanel& operator=(const GpuInfoPanel&) = delete;

    /// \param [in] pFactory      - Engine factory used to enumerate adapters on non-Windows platforms.
    /// \param [in] ppAppDevices  - Devices the application actually renders with. Used to flag which
    ///                             GPUs are in use and as a fallback source of adapter information.
    /// \param [in] NumAppDevices - Number of devices in ppAppDevices.
    void Initialize(IEngineFactory* pFactory, IRenderDevice* const* ppAppDevices, Uint32 NumAppDevices);

    /// Refreshes the live statistics. Call once per frame; the work is internally throttled.
    void Update(double ElapsedTime);

    /// Emits the ImGui commands for the panel. Must be called inside an active ImGui frame.
    void UpdateUI(const char* Title, bool* pOpen = nullptr);

private:
    void RefreshLiveStats();

    struct GpuInfo
    {
        std::string Name;
        std::string Type;

        Uint64 TotalVRAM  = 0; // Dedicated video memory, in bytes.
        Uint64 UsedVRAM   = 0; // Current usage by this process, in bytes.
        Uint64 BudgetVRAM = 0; // Memory budget for this process, in bytes.

        double GpuUsagePercent = -1.0; // < 0 means "not available".

        bool HasLiveVRAM = false;
        bool UsedByApp   = false;
    };

    std::vector<GpuInfo> m_Gpus;

    double m_TimeSinceRefresh = 1.0e9; // Force a refresh on the first frame.
    double m_RefreshInterval  = 0.5;

    // Platform-specific state (DXGI adapters + PDH query on Windows).
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Diligent
