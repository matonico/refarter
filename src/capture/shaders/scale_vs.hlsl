// One triangle covering the viewport; the pixel shader works from SV_Position alone.
float4 main(uint id : SV_VertexID) : SV_Position {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
