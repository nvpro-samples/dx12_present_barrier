// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

cbuffer LineConstants : register(b0)
{
    float3 color;
    float unused[7];
};

static const float4 positions[] =
{
    float4(-0.295f, 0.95f, 0.5f, 1.0f),
    float4( 0.295f, 0.95f, 0.5f, 1.0f),
    float4(-0.300f, 0.94f, 0.5f, 1.0f),
    float4( 0.300f, 0.94f, 0.5f, 1.0f),
    float4(-0.300f, 0.93f, 0.5f, 1.0f),
    float4( 0.300f, 0.93f, 0.5f, 1.0f),
    float4(-0.295f, 0.92f, 0.5f, 1.0f),
    float4( 0.295f, 0.92f, 0.5f, 1.0f),
};

void main(uint idx : SV_VertexID, out float4 position : SV_Position, out float3 col : LINE_COLOR)
{
    col = color;
    position = positions[idx];
}
