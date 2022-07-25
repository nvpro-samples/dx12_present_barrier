// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

float4 main(in float4 pos : SV_Position, in float3 col : LINE_COLOR) : SV_Target
{
  return float4(col, 1.0);
}
