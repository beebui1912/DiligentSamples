# Tutorial32 - Unlinked Multi-GPU

This tutorial demonstrates **Unlinked Multi-GPU** rendering, where two *independent* devices (which
may live on two different physical GPUs, or on the same GPU) cooperate to produce one frame.

Unlike linked mode, the devices do **not** share memory, so a render result produced on one device
cannot be sampled directly by the other. The data must travel GPU -> CPU -> GPU. Diligent provides
the `CrossDeviceTransferManager` helper (in `Diligent-GraphicsTools`) to pipeline this transfer.

## Overview

- The **primary device** is created by the sample framework. It owns the swap chain and renders the
  left half of the screen (a rotating cube).
- The sample creates a **secondary device** itself (via `IEngineFactoryD3D12`/`IEngineFactoryVk`),
  preferring a different adapter when the system exposes more than one. It renders the right half
  into its own render target.
- Each frame the secondary result is copied to a host-visible staging texture, read back to the CPU,
  written into a staging texture on the primary device, and copied into a primary-device texture -
  all handled by `CrossDeviceTransferManager::CopySourceFrame` / `TransferReadyFrame`.
- The primary device then composes both halves into the swap chain back buffer.

A per-device color tint (reddish for the primary, bluish for the secondary) makes it clear which
device produced each half.

## Multi-GPU API used

| API element | Purpose |
|-------------|---------|
| `IEngineFactoryD3D12::CreateDeviceAndContextsD3D12` / `IEngineFactoryVk::CreateDeviceAndContextsVk` | Creates a second independent device |
| `EngineCreateInfo::AdapterId` | Selects which adapter the secondary device runs on |
| `CrossDeviceTransferManager` | Pipelines the GPU -> CPU -> GPU transfer between devices |
| `USAGE_STAGING` textures + `MapTextureSubresource` | Host-visible staging used by the transfer |

## Single-GPU / single-device behavior

If a secondary device cannot be created (for example, on a backend that is not D3D12 or Vulkan), the
sample falls back to a single view and renders the whole scene on the primary device.

On a machine with a single GPU, the secondary device is created on the *same* physical GPU. This is
fully valid and still exercises the complete cross-device transfer path, so the sample is useful for
development even without a second GPU.

## Vulkan note

Diligent's Vulkan backend uses [volk](https://github.com/zeux/volk). `volkLoadInstance()` loads
every device-level entry point as a loader trampoline that dispatches correctly for **any** device,
while `volkLoadDevice()` rebinds the single set of *global* pointers to one specific device for a
small speedup. To let more than one Vulkan device coexist and stay fast, each
`VulkanUtilities::LogicalDevice` now owns a dedicated `VolkDeviceTable` (via `volkLoadDeviceTable()`),
and the per-frame hot path - command-buffer recording (`VulkanUtilities::CommandBuffer`) - dispatches
through the owning device's table (`LogicalDevice::GetVkTable()`). This is the same technique
The-Forge uses, so every device records commands through its own optimized, non-trampolined function
pointers. Cold-path calls that are not routed through a table fall back to the global pointers, which
remain valid for any device. Single-device applications keep their fully optimized path.

> **Note:** Creating a second device is wired up for the Direct3D12 and Vulkan backends. See
> `Tutorial31_LinkedMultiGPU` for multi-GPU using a single linked device with multiple nodes.
