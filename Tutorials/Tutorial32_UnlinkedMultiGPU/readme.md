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

> **Note:** Creating a second device is currently wired up for the Direct3D12 and Vulkan backends.
> See `Tutorial31_LinkedMultiGPU` for multi-GPU using a single linked device with multiple nodes.
