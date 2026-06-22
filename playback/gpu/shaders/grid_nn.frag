#version 440

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

const int kMaxGridSources = 16;

// Oracle-validated compat lane. The uniforms mirror formatcanon's
// referenceComposeGridRgba8 contract: NV12 nearest sampling, then integer
// yuvToRgb8 coefficients. Matrix: 0=BT.601, 1=BT.709. Range: 0=Full, 1=Video.
layout(std140, binding = 0) uniform GridUniforms
{
    int uMatrix;
    int uRange;
    int uColumns;
    int uRows;
    ivec4 uSourceSize[kMaxGridSources]; // width, height, present, unused
}
ub;

layout(binding = 1) uniform texture2D texLuma0;
layout(binding = 2) uniform texture2D texChroma0;
layout(binding = 3) uniform texture2D texLuma1;
layout(binding = 4) uniform texture2D texChroma1;
layout(binding = 5) uniform texture2D texLuma2;
layout(binding = 6) uniform texture2D texChroma2;
layout(binding = 7) uniform texture2D texLuma3;
layout(binding = 8) uniform texture2D texChroma3;
layout(binding = 9) uniform texture2D texLuma4;
layout(binding = 10) uniform texture2D texChroma4;
layout(binding = 11) uniform texture2D texLuma5;
layout(binding = 12) uniform texture2D texChroma5;
layout(binding = 13) uniform texture2D texLuma6;
layout(binding = 14) uniform texture2D texChroma6;
layout(binding = 15) uniform texture2D texLuma7;
layout(binding = 16) uniform texture2D texChroma7;
layout(binding = 17) uniform texture2D texLuma8;
layout(binding = 18) uniform texture2D texChroma8;
layout(binding = 19) uniform texture2D texLuma9;
layout(binding = 20) uniform texture2D texChroma9;
layout(binding = 21) uniform texture2D texLuma10;
layout(binding = 22) uniform texture2D texChroma10;
layout(binding = 23) uniform texture2D texLuma11;
layout(binding = 24) uniform texture2D texChroma11;
layout(binding = 25) uniform texture2D texLuma12;
layout(binding = 26) uniform texture2D texChroma12;
layout(binding = 27) uniform texture2D texLuma13;
layout(binding = 28) uniform texture2D texChroma13;
layout(binding = 29) uniform texture2D texLuma14;
layout(binding = 30) uniform texture2D texChroma14;
layout(binding = 31) uniform texture2D texLuma15;
layout(binding = 32) uniform texture2D texChroma15;
layout(binding = 33) uniform sampler texSampler;

int clampU8(int v)
{
    return clamp(v, 0, 255);
}

int texelU8(vec4 texel, int channel)
{
    return int(round(texel[channel] * 255.0));
}

vec4 lumaTexel(int source, ivec2 coord)
{
    if (source == 0)
        return texelFetch(sampler2D(texLuma0, texSampler), coord, 0);
    if (source == 1)
        return texelFetch(sampler2D(texLuma1, texSampler), coord, 0);
    if (source == 2)
        return texelFetch(sampler2D(texLuma2, texSampler), coord, 0);
    if (source == 3)
        return texelFetch(sampler2D(texLuma3, texSampler), coord, 0);
    if (source == 4)
        return texelFetch(sampler2D(texLuma4, texSampler), coord, 0);
    if (source == 5)
        return texelFetch(sampler2D(texLuma5, texSampler), coord, 0);
    if (source == 6)
        return texelFetch(sampler2D(texLuma6, texSampler), coord, 0);
    if (source == 7)
        return texelFetch(sampler2D(texLuma7, texSampler), coord, 0);
    if (source == 8)
        return texelFetch(sampler2D(texLuma8, texSampler), coord, 0);
    if (source == 9)
        return texelFetch(sampler2D(texLuma9, texSampler), coord, 0);
    if (source == 10)
        return texelFetch(sampler2D(texLuma10, texSampler), coord, 0);
    if (source == 11)
        return texelFetch(sampler2D(texLuma11, texSampler), coord, 0);
    if (source == 12)
        return texelFetch(sampler2D(texLuma12, texSampler), coord, 0);
    if (source == 13)
        return texelFetch(sampler2D(texLuma13, texSampler), coord, 0);
    if (source == 14)
        return texelFetch(sampler2D(texLuma14, texSampler), coord, 0);
    return texelFetch(sampler2D(texLuma15, texSampler), coord, 0);
}

vec4 chromaTexel(int source, ivec2 coord)
{
    if (source == 0)
        return texelFetch(sampler2D(texChroma0, texSampler), coord, 0);
    if (source == 1)
        return texelFetch(sampler2D(texChroma1, texSampler), coord, 0);
    if (source == 2)
        return texelFetch(sampler2D(texChroma2, texSampler), coord, 0);
    if (source == 3)
        return texelFetch(sampler2D(texChroma3, texSampler), coord, 0);
    if (source == 4)
        return texelFetch(sampler2D(texChroma4, texSampler), coord, 0);
    if (source == 5)
        return texelFetch(sampler2D(texChroma5, texSampler), coord, 0);
    if (source == 6)
        return texelFetch(sampler2D(texChroma6, texSampler), coord, 0);
    if (source == 7)
        return texelFetch(sampler2D(texChroma7, texSampler), coord, 0);
    if (source == 8)
        return texelFetch(sampler2D(texChroma8, texSampler), coord, 0);
    if (source == 9)
        return texelFetch(sampler2D(texChroma9, texSampler), coord, 0);
    if (source == 10)
        return texelFetch(sampler2D(texChroma10, texSampler), coord, 0);
    if (source == 11)
        return texelFetch(sampler2D(texChroma11, texSampler), coord, 0);
    if (source == 12)
        return texelFetch(sampler2D(texChroma12, texSampler), coord, 0);
    if (source == 13)
        return texelFetch(sampler2D(texChroma13, texSampler), coord, 0);
    if (source == 14)
        return texelFetch(sampler2D(texChroma14, texSampler), coord, 0);
    return texelFetch(sampler2D(texChroma15, texSampler), coord, 0);
}

vec4 yuvToRgba8(int y, int u, int v)
{
    int yp = (ub.uRange == 1) ? (y - 16) * 1192 : y * 1024;
    int cb = u - 128;
    int cr = v - 128;

    int r;
    int g;
    int b;
    if (ub.uMatrix == 1) {
        r = yp + 1836 * cr;
        g = yp - 218 * cb - 546 * cr;
        b = yp + 2164 * cb;
    } else {
        r = yp + 1634 * cr;
        g = yp - 401 * cb - 832 * cr;
        b = yp + 2066 * cb;
    }

    return vec4(float(clampU8((r + 512) >> 10)) / 255.0,
                float(clampU8((g + 512) >> 10)) / 255.0,
                float(clampU8((b + 512) >> 10)) / 255.0, 1.0);
}

void main()
{
    if (ub.uColumns <= 0 || ub.uRows <= 0) {
        fragColor = yuvToRgba8(16, 128, 128);
        return;
    }

    vec2 uv = clamp(vUv, vec2(0.0), vec2(0.999999));
    int col = min(ub.uColumns - 1, int(floor(uv.x * float(ub.uColumns))));
    int row = min(ub.uRows - 1, int(floor(uv.y * float(ub.uRows))));
    int source = row * ub.uColumns + col;
    if (source < 0 || source >= kMaxGridSources || ub.uSourceSize[source].z == 0) {
        fragColor = yuvToRgba8(16, 128, 128);
        return;
    }

    int srcW = ub.uSourceSize[source].x;
    int srcH = ub.uSourceSize[source].y;
    if (srcW <= 0 || srcH <= 0) {
        fragColor = yuvToRgba8(16, 128, 128);
        return;
    }

    vec2 tileUv = vec2(uv.x * float(ub.uColumns) - float(col),
                       uv.y * float(ub.uRows) - float(row));
    int lx = min(srcW - 1, max(0, int(tileUv.x * float(srcW))));
    int ly = min(srcH - 1, max(0, int(tileUv.y * float(srcH))));
    int cx = lx / 2;
    int cy = ly / 2;

    vec4 yTexel = lumaTexel(source, ivec2(lx, ly));
    vec4 uvTexel = chromaTexel(source, ivec2(cx, cy));
    fragColor = yuvToRgba8(texelU8(yTexel, 0), texelU8(uvTexel, 0), texelU8(uvTexel, 1));
}
