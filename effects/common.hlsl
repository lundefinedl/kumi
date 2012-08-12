Texture2D Texture0 : register(t0);
Texture2D Texture1 : register(t1);
Texture2D Texture2 : register(t2);
Texture2D Texture3 : register(t3);

sampler LinearSampler;
sampler PointSampler;

struct quad_vs_input {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

struct quad_ps_input {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

quad_ps_input quad_vs_main(quad_vs_input input)
{
    quad_ps_input output = (quad_ps_input)0;
    output.pos = input.pos;
    output.tex = input.tex;
    return output;
}