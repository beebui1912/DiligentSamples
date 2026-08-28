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

#include "GpuInfoPanel.hpp"

#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "GraphicsAccessories.hpp"

#if PLATFORM_WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <dxgi1_4.h>
#    include <pdh.h>
#    include <pdhmsg.h>
#    include <wrl/client.h>
#    pragma comment(lib, "dxgi.lib")
#    pragma comment(lib, "pdh.lib")
#endif

namespace Diligent
{

namespace
{

const char* FormatMemoryMB(char* Buffer, size_t BufferSize, Uint64 Used, Uint64 Total)
{
    const double UsedMB  = static_cast<double>(Used) / (1024.0 * 1024.0);
    const double TotalMB = static_cast<double>(Total) / (1024.0 * 1024.0);
    std::snprintf(Buffer, BufferSize, "%.0f / %.0f MB", UsedMB, TotalMB);
    return Buffer;
}

} // namespace

#if PLATFORM_WIN32

namespace
{

std::string WideToUtf8(const wchar_t* Wide)
{
    if (Wide == nullptr)
        return {};
    const int Len = ::WideCharToMultiByte(CP_UTF8, 0, Wide, -1, nullptr, 0, nullptr, nullptr);
    if (Len <= 1)
        return {};
    std::string Result(static_cast<size_t>(Len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, Wide, -1, &Result[0], Len, nullptr, nullptr);
    return Result;
}

// Parses a "GPU Engine" PDH instance name of the form
// "pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D".
bool ParseGpuEngineInstance(const wchar_t* Name, DWORD& Pid, Uint32& LuidHigh, Uint32& LuidLow)
{
    const wchar_t* pPid = std::wcsstr(Name, L"pid_");
    if (pPid == nullptr || swscanf_s(pPid, L"pid_%u", &Pid) != 1)
        return false;

    const wchar_t* pLuid = std::wcsstr(Name, L"luid_0x");
    unsigned int   High  = 0;
    unsigned int   Low   = 0;
    if (pLuid == nullptr || swscanf_s(pLuid, L"luid_0x%x_0x%x", &High, &Low) != 2)
        return false;

    LuidHigh = High;
    LuidLow  = Low;
    return true;
}

} // namespace

struct GpuInfoPanel::Impl
{
    Microsoft::WRL::ComPtr<IDXGIFactory4>              Factory;
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter3>> Adapters; // Aligned with GpuInfoPanel::m_Gpus.
    std::vector<std::pair<Uint32, Uint32>>             Luids;     // {HighPart, LowPart}, aligned with m_Gpus.

    PDH_HQUERY   PdhQuery   = nullptr;
    PDH_HCOUNTER PdhCounter = nullptr;
    bool         PdhReady   = false;
    DWORD        Pid        = 0;

    std::vector<unsigned char> PdhBuffer;

    ~Impl()
    {
        if (PdhQuery != nullptr)
            PdhCloseQuery(PdhQuery);
    }
};

#else

struct GpuInfoPanel::Impl
{
};

#endif // PLATFORM_WIN32

GpuInfoPanel::GpuInfoPanel() :
    m_Impl{std::make_unique<Impl>()}
{
}

GpuInfoPanel::~GpuInfoPanel() = default;

void GpuInfoPanel::Initialize(IEngineFactory* pFactory, IRenderDevice* const* ppAppDevices, Uint32 NumAppDevices)
{
    m_Gpus.clear();

    // Collect the adapters the application actually renders with (name + type).
    struct AppAdapter
    {
        std::string Name;
        std::string Type;
    };
    std::vector<AppAdapter> AppAdapters;
    for (Uint32 i = 0; i < NumAppDevices; ++i)
    {
        if (ppAppDevices[i] == nullptr)
            continue;
        const GraphicsAdapterInfo& AI = ppAppDevices[i]->GetAdapterInfo();
        AppAdapters.push_back({AI.Description, GetAdapterTypeString(AI.Type)});
    }

#if PLATFORM_WIN32
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(m_Impl->Factory.GetAddressOf()))))
    {
        using Microsoft::WRL::ComPtr;
        for (UINT AdapterId = 0;; ++AdapterId)
        {
            ComPtr<IDXGIAdapter1> Adapter1;
            if (m_Impl->Factory->EnumAdapters1(AdapterId, Adapter1.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND)
                break;

            DXGI_ADAPTER_DESC1 Desc{};
            Adapter1->GetDesc1(&Desc);

            GpuInfo Gpu;
            Gpu.Name      = WideToUtf8(Desc.Description);
            Gpu.TotalVRAM = Desc.DedicatedVideoMemory;
            if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                Gpu.Type = "Software";
            else if (Desc.DedicatedVideoMemory >= static_cast<SIZE_T>(512) * 1024 * 1024)
                Gpu.Type = "Discrete";
            else
                Gpu.Type = "Integrated";

            for (const AppAdapter& App : AppAdapters)
            {
                if (App.Name == Gpu.Name)
                {
                    Gpu.UsedByApp = true;
                    if (!App.Type.empty())
                        Gpu.Type = App.Type;
                    break;
                }
            }

            ComPtr<IDXGIAdapter3> Adapter3;
            m_Impl->Adapters.push_back(SUCCEEDED(Adapter1.As(&Adapter3)) ? Adapter3 : nullptr);
            m_Impl->Luids.push_back({static_cast<Uint32>(Desc.AdapterLuid.HighPart), static_cast<Uint32>(Desc.AdapterLuid.LowPart)});
            m_Gpus.push_back(std::move(Gpu));
        }
    }

    // Set up the PDH query for per-process GPU utilization.
    m_Impl->Pid = GetCurrentProcessId();
    if (PdhOpenQueryW(nullptr, 0, &m_Impl->PdhQuery) == ERROR_SUCCESS)
    {
        if (PdhAddEnglishCounterW(m_Impl->PdhQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_Impl->PdhCounter) == ERROR_SUCCESS)
        {
            // Prime the query so the next collection produces a valid delta.
            PdhCollectQueryData(m_Impl->PdhQuery);
            m_Impl->PdhReady = true;
        }
    }
#endif // PLATFORM_WIN32

    // Fallback: if no adapters were discovered above, use the application's own device information.
    if (m_Gpus.empty())
    {
        for (const AppAdapter& App : AppAdapters)
        {
            GpuInfo Gpu;
            Gpu.Name      = App.Name;
            Gpu.Type      = App.Type;
            Gpu.UsedByApp = true;
            m_Gpus.push_back(std::move(Gpu));
        }

        // Pull total VRAM from the app devices.
        Uint32 Idx = 0;
        for (Uint32 i = 0; i < NumAppDevices; ++i)
        {
            if (ppAppDevices[i] == nullptr)
                continue;
            if (Idx < m_Gpus.size())
                m_Gpus[Idx].TotalVRAM = ppAppDevices[i]->GetAdapterInfo().Memory.LocalMemory;
            ++Idx;
        }
    }
}

void GpuInfoPanel::RefreshLiveStats()
{
#if PLATFORM_WIN32
    // Live VRAM usage per adapter.
    for (size_t i = 0; i < m_Gpus.size() && i < m_Impl->Adapters.size(); ++i)
    {
        if (!m_Impl->Adapters[i])
            continue;

        DXGI_QUERY_VIDEO_MEMORY_INFO MemInfo{};
        if (SUCCEEDED(m_Impl->Adapters[i]->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &MemInfo)))
        {
            m_Gpus[i].UsedVRAM    = MemInfo.CurrentUsage;
            m_Gpus[i].BudgetVRAM  = MemInfo.Budget;
            m_Gpus[i].HasLiveVRAM = true;
        }
    }

    // Per-process GPU utilization via PDH.
    if (m_Impl->PdhReady && PdhCollectQueryData(m_Impl->PdhQuery) == ERROR_SUCCESS)
    {
        DWORD      BufferSize = 0;
        DWORD      ItemCount  = 0;
        PDH_STATUS Status     = PdhGetFormattedCounterArrayW(m_Impl->PdhCounter, PDH_FMT_DOUBLE, &BufferSize, &ItemCount, nullptr);
        if (Status == PDH_MORE_DATA && BufferSize > 0)
        {
            m_Impl->PdhBuffer.resize(BufferSize);
            auto* pItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(m_Impl->PdhBuffer.data());
            Status       = PdhGetFormattedCounterArrayW(m_Impl->PdhCounter, PDH_FMT_DOUBLE, &BufferSize, &ItemCount, pItems);
            if (Status == ERROR_SUCCESS)
            {
                // Reset accumulators (0% is a valid "not using this GPU" reading).
                for (GpuInfo& Gpu : m_Gpus)
                    Gpu.GpuUsagePercent = 0.0;

                for (DWORD i = 0; i < ItemCount; ++i)
                {
                    if (pItems[i].FmtValue.CStatus != ERROR_SUCCESS)
                        continue;

                    DWORD  Pid      = 0;
                    Uint32 LuidHigh = 0;
                    Uint32 LuidLow  = 0;
                    if (!ParseGpuEngineInstance(pItems[i].szName, Pid, LuidHigh, LuidLow))
                        continue;
                    if (Pid != m_Impl->Pid)
                        continue;

                    for (size_t g = 0; g < m_Gpus.size() && g < m_Impl->Luids.size(); ++g)
                    {
                        if (m_Impl->Luids[g].first == LuidHigh && m_Impl->Luids[g].second == LuidLow)
                        {
                            m_Gpus[g].GpuUsagePercent += pItems[i].FmtValue.doubleValue;
                            break;
                        }
                    }
                }

                // Engine utilizations are summed, so clamp to a sensible display range.
                for (GpuInfo& Gpu : m_Gpus)
                {
                    if (Gpu.GpuUsagePercent > 100.0)
                        Gpu.GpuUsagePercent = 100.0;
                }
            }
        }
    }
#endif // PLATFORM_WIN32
}

void GpuInfoPanel::Update(double ElapsedTime)
{
    m_TimeSinceRefresh += ElapsedTime;
    if (m_TimeSinceRefresh >= m_RefreshInterval)
    {
        m_TimeSinceRefresh = 0.0;
        RefreshLiveStats();
    }
}

void GpuInfoPanel::UpdateUI(const char* Title, bool* pOpen)
{
    ImGui::SetNextWindowPos(ImVec2(10, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(Title, pOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_Gpus.empty())
        {
            ImGui::TextUnformatted("No GPU information available.");
        }

        for (size_t i = 0; i < m_Gpus.size(); ++i)
        {
            const GpuInfo& Gpu = m_Gpus[i];

            ImGui::PushID(static_cast<int>(i));

            if (Gpu.UsedByApp)
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s  [in use]", Gpu.Name.c_str());
            else
                ImGui::TextUnformatted(Gpu.Name.c_str());

            ImGui::Text("Type: %s", Gpu.Type.empty() ? "Unknown" : Gpu.Type.c_str());

            // VRAM
            char Overlay[64];
            if (Gpu.HasLiveVRAM)
            {
                const Uint64 Denom = Gpu.TotalVRAM > 0 ? Gpu.TotalVRAM : Gpu.BudgetVRAM;
                float        Frac  = Denom > 0 ? static_cast<float>(static_cast<double>(Gpu.UsedVRAM) / static_cast<double>(Denom)) : 0.0f;
                if (Frac < 0.0f) Frac = 0.0f;
                if (Frac > 1.0f) Frac = 1.0f;
                FormatMemoryMB(Overlay, sizeof(Overlay), Gpu.UsedVRAM, Denom);
                ImGui::Text("VRAM:");
                ImGui::SameLine();
                ImGui::ProgressBar(Frac, ImVec2(-1.0f, 0.0f), Overlay);
            }
            else if (Gpu.TotalVRAM > 0)
            {
                ImGui::Text("VRAM: %.0f MB total", static_cast<double>(Gpu.TotalVRAM) / (1024.0 * 1024.0));
            }
            else
            {
                ImGui::TextUnformatted("VRAM: N/A");
            }

            // GPU utilization
            if (Gpu.GpuUsagePercent >= 0.0)
            {
                std::snprintf(Overlay, sizeof(Overlay), "%.0f%%", Gpu.GpuUsagePercent);
                ImGui::Text("GPU: ");
                ImGui::SameLine();
                ImGui::ProgressBar(static_cast<float>(Gpu.GpuUsagePercent / 100.0), ImVec2(-1.0f, 0.0f), Overlay);
            }
            else
            {
                ImGui::TextUnformatted("GPU:  N/A");
            }

            if (i + 1 < m_Gpus.size())
                ImGui::Separator();

            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace Diligent
