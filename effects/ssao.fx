matrix proj, view, world;
float4 Diffuse, LightColor, LightPos;

///////////////////////////////////
// g-buffer fill
///////////////////////////////////
struct fill_vs_input {
    float4 pos : POSITION;
    float3 normal : NORMAL;
    float2 tex : TEXCOORD;
};

struct fill_ps_input {
    float4 pos : SV_POSITION;
    float4 vs_pos : TEXCOORD0;
    float4 vs_normal : TEXCOORD1;
};

struct fill_ps_output {
    float4 rt0 : COLOR1;
    float4 rt1 : COLOR2;
};

fill_ps_input fill_vs_main(fill_vs_input input)
{
    fill_ps_input output = (fill_ps_input)0;
    float4x4 world_view = mul(world, view);
    output.pos = mul(input.pos, mul(world_view, proj));
    output.vs_pos = mul(input.pos, world_view);
    output.vs_normal = mul(float4(input.normal,0), world_view);
    return output;
}

fill_ps_output fill_ps_main(fill_ps_input input) : SV_Target
{
    fill_ps_output output = (fill_ps_output)0;
    output.rt0 = input.vs_pos;
    output.rt1 = normalize(input.vs_normal);
    
    float n = 1;
    float f = 2500;
    output.rt0.w = (output.rt0.z - n) / (f - n);

    return output;
}

///////////////////////////////////
// actual render
///////////////////////////////////

Texture2D rt_pos : register(t0);
Texture2D rt_normal : register(t1);
Texture2D random_normals : register(t2);
sampler ssao_sampler : register(s0);

float4 kernel[32];
float4 noise[16];

struct render_vs_input {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

struct render_ps_input {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

render_ps_input render_vs_main(render_vs_input input)
{
    render_ps_input output = (render_ps_input)0;
    output.pos = input.pos;
    output.tex = input.tex;
    return output;
}

float4 render_ps_main(render_ps_input input) : SV_Target
{
    float3 origin = rt_pos.Sample(ssao_sampler, input.tex).xyz;
    float3 normal = rt_normal.Sample(ssao_sampler, input.tex).xyz;
    
    // tile the noise in a 4x4 grid
    int x = (int)(960.0 * input.tex.x);
    int y = (int)(600.0 * input.tex.y);
    int idx = (y%4)*4 + (x%4);
    float3 rvec = noise[idx].xyz;
    
    // Gram-Schmidt
    float3 tangent = normalize(rvec - normal * dot(rvec, normal));
    float3 bitangent = cross(normal, tangent);
    
    float3x3 tbn = float3x3( 
        tangent.x, tangent.y, tangent.z,
        bitangent.x, bitangent.y, bitangent.z,
        normal.x, normal.y, normal.z);
        
    float occlusion = 0.0;
    float radius = 27;
    int KERNEL_SIZE = 32;
    for (int i = 0; i < KERNEL_SIZE; ++i) {
        // get sample position
        float3 sample = mul(kernel[i].xyz, tbn);
        sample = sample * radius + origin;
        
        // project sample position
        float4 offset = float4(sample, 1.0);
        offset = mul(offset, proj); // to clip space
        offset.xy /= offset.w; // to NDC
        offset.xy = offset.xy * 0.5 + 0.5; // to texture coords
        offset.y = 1 - offset.y;
        
        float sample_depth = rt_pos.Sample(ssao_sampler, offset.xy).z;

        // check for big discontinuities in z
        float range_check = abs(origin.z - sample_depth) < radius ? 1.0 : 0.0;
        occlusion += (sample_depth <= sample.z ? 1.0 : 0.0) * range_check;
    }
    
    return origin.z > 0 ? 1.0 - (occlusion / KERNEL_SIZE) : 0;
}

///////////////////////////////////
// blur
///////////////////////////////////

Texture2D rt_blur : register(t0);

struct blur_vs_input {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

struct blur_ps_input {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

blur_ps_input blur_vs_main(blur_vs_input input)
{
    blur_ps_input output = (blur_ps_input)0;
    output.pos = input.pos;
    output.tex = input.tex;
    return output;
}

float4 blur_ps_main(blur_ps_input input) : SV_Target
{
    float res = 0.0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float2 ofs = float2(1/960.0 * j, 1/600.0*i);
            res += rt_blur.Sample(ssao_sampler, input.tex + ofs).r;
        }
    }
    return res / 16.0;
}