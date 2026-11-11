// Fullscreen gradient drawn by the main.cpp demo loop; a quick target for testing shader reload.

static const float3 top_color    = float3(0.10f, 0.12f, 0.30f);
static const float3 bottom_color = float3(0.55f, 0.35f, 0.25f);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// One triangle covering the screen: vertices (-1,-1), (3,-1), (-1,3).
[shader("vertex")]
VertexOutput vertexMain(uint vertex_index : SV_VertexID)
{
    const float2 uv = float2((vertex_index << 1) & 2, vertex_index & 2);
    VertexOutput output;
    output.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    return float4(lerp(top_color, bottom_color, saturate(input.uv.y)), 1.0f);
}
