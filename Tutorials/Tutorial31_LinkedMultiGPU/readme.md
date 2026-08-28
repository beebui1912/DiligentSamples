# Tutorial31 - Linked Multi-GPU

This tutorial demonstrates **Linked Multi-GPU** (also known as Linked Display Adapter / LDA in
Direct3D12, or a Device Group in Vulkan) split-frame rendering.

In linked mode a single logical `IRenderDevice` drives several physical GPU **nodes** that are
bridged by the driver (for example, an NVLink or Crossfire pair of identical GPUs). Each node has
its own memory pool but shares a single command/resource model.

## Overview

The screen is divided into vertical strips - one per GPU node:

- Each node renders its strip (a rotating cube on a distinct background tint) into its **own**
  off-screen color/depth render target, allocated on that node via
  `TextureDesc::CreationNodeMask` and made visible to node 0 via `TextureDesc::VisibleNodeMask`.
- Every node runs on its own immediate context, created with a distinct
  `ImmediateContextCreateInfo::NodeIndex`.
- Node 0 then waits for the other nodes (using an `IFence`) and composes all strips into the swap
  chain back buffer with a full-screen blit.

A per-node color tint (reddish for node 0, bluish for node 1) makes it easy to see which GPU
produced each strip.

## Multi-GPU API used

| API element | Purpose |
|-------------|---------|
| `EngineCreateInfo::GpuMode = GPU_MODE_LINKED` | Requests linked multi-GPU device creation |
| `EngineCreateInfo::NodeCount`                 | Number of linked nodes to use |
| `GraphicsAdapterInfo::NodeCount / NodeMask`   | Reports how many linked nodes the adapter has |
| `ImmediateContextCreateInfo::NodeIndex`       | Binds an immediate context to a GPU node |
| `TextureDesc::CreationNodeMask / VisibleNodeMask` | Controls where a resource lives and which nodes can read it |

## Single-GPU behavior

Most systems expose a single GPU node. In that case the sample automatically falls back to a
single view and renders the whole scene on one node - the split/compose structure is preserved
but only one strip is produced. The multi-node path is exercised automatically when the D3D12 or
Vulkan device reports more than one linked node.

> **Note:** Linked multi-GPU requires the Direct3D12 or Vulkan backend. Direct3D11, OpenGL, Metal
> and WebGPU always run the single-node path. See `Tutorial32_UnlinkedMultiGPU` for multi-GPU
> across *independent* devices/adapters.
