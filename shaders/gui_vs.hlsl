// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

float4 main(uint vid
            : SV_VertexID)
    : SV_Position
{
  switch(vid)
  {
    case 0:
      return float4(-1.0f, -1.0f, 0.0f, 1.0f);
    case 1:
      return float4(-1.0f, 3.0f, 0.0f, 1.0f);
    case 2:
      return float4(3.0f, -1.0f, 0.0f, 1.0f);
    default:
      return 0.0f;
  }
}