// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

Texture2D<float4> g_texture : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target {
  return g_texture.Load(int3(pos.xy, 0));
}