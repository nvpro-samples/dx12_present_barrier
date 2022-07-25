// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

cbuffer LineConstants : register(b0)
{
  float verticalSizeA;
  float verticalSizeB;
  float horizontalSizeA;
  float horizontalSizeB;
  float verticalOffset;
  float horizontalOffset;
  float verticalSpacing;
  float horizontalSpacing;
  uint1 numLines;
  uint1 firstHorizontalInstance;
  uint1 extraOffset;
};

void main(uint idx : SV_VertexID, uint instance : SV_InstanceID, out float4 position : SV_Position, out float3 col : LINE_COLOR)
{
  const bool horizontal = instance >= firstHorizontalInstance;
  col = horizontal ? float3(extraOffset ? 1 : 0, 1, 0) : float3(1, 0, extraOffset ? 1 : 0);

  float offset = horizontal ? horizontalOffset : verticalOffset;
  // Add an offset for every instance so lines have some spacing between them
  offset += horizontal ? (instance - firstHorizontalInstance) * horizontalSpacing : instance * verticalSpacing;
  // Wrap around at the screen borders
  offset  = fmod(offset, 1.0);

  // Variate the line size every few lines break up the pattern
  const float size = horizontal ?
    ((instance % 3) == 0 ? horizontalSizeB : horizontalSizeA) : ((instance % 3) == 0 ? verticalSizeB : verticalSizeA);

  // Generate vertices for a quad that stretches across the screen in one dimension and has a fixed size in the other dimension
  float a = (idx < 2 ? -1.0 : 1.0);
  float b = (idx % 2 ? -size : size) + (offset * 2 - 1);
  position = horizontal ? float4(a, b, 0.5, 1.0) : float4(b, a, 0.5, 1.0);
}
