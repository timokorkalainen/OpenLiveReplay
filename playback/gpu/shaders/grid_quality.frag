#version 440

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

const int kMaxGridSources = 16;

// Quality scaler lane. The uniforms mirror formatcanon's
// referenceComposeGridRgba8 contract: NV12 bilinear sampling, then integer
// yuvToRgb8 coefficients. Matrix: 0=BT.601, 1=BT.709. Range: 0=Full, 1=Video.
layout(std140, binding = 0) uniform GridUniforms
{
    int uMatrix;
    int uRange;
    int uColumns;
    int uRows;
    ivec4 uOutputSize;              // width, height, unused, unused
    ivec4 uSourceSize[kMaxGridSources]; // width, height, present, unused
    ivec4 uTileRect[kMaxGridSources];   // dstX, dstY, dstW, dstH
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

vec4 lumaSample(int source, vec2 uv)
{
    if (source == 0)
        return texture(sampler2D(texLuma0, texSampler), uv);
    if (source == 1)
        return texture(sampler2D(texLuma1, texSampler), uv);
    if (source == 2)
        return texture(sampler2D(texLuma2, texSampler), uv);
    if (source == 3)
        return texture(sampler2D(texLuma3, texSampler), uv);
    if (source == 4)
        return texture(sampler2D(texLuma4, texSampler), uv);
    if (source == 5)
        return texture(sampler2D(texLuma5, texSampler), uv);
    if (source == 6)
        return texture(sampler2D(texLuma6, texSampler), uv);
    if (source == 7)
        return texture(sampler2D(texLuma7, texSampler), uv);
    if (source == 8)
        return texture(sampler2D(texLuma8, texSampler), uv);
    if (source == 9)
        return texture(sampler2D(texLuma9, texSampler), uv);
    if (source == 10)
        return texture(sampler2D(texLuma10, texSampler), uv);
    if (source == 11)
        return texture(sampler2D(texLuma11, texSampler), uv);
    if (source == 12)
        return texture(sampler2D(texLuma12, texSampler), uv);
    if (source == 13)
        return texture(sampler2D(texLuma13, texSampler), uv);
    if (source == 14)
        return texture(sampler2D(texLuma14, texSampler), uv);
    return texture(sampler2D(texLuma15, texSampler), uv);
}

vec4 chromaSample(int source, vec2 uv)
{
    if (source == 0)
        return texture(sampler2D(texChroma0, texSampler), uv);
    if (source == 1)
        return texture(sampler2D(texChroma1, texSampler), uv);
    if (source == 2)
        return texture(sampler2D(texChroma2, texSampler), uv);
    if (source == 3)
        return texture(sampler2D(texChroma3, texSampler), uv);
    if (source == 4)
        return texture(sampler2D(texChroma4, texSampler), uv);
    if (source == 5)
        return texture(sampler2D(texChroma5, texSampler), uv);
    if (source == 6)
        return texture(sampler2D(texChroma6, texSampler), uv);
    if (source == 7)
        return texture(sampler2D(texChroma7, texSampler), uv);
    if (source == 8)
        return texture(sampler2D(texChroma8, texSampler), uv);
    if (source == 9)
        return texture(sampler2D(texChroma9, texSampler), uv);
    if (source == 10)
        return texture(sampler2D(texChroma10, texSampler), uv);
    if (source == 11)
        return texture(sampler2D(texChroma11, texSampler), uv);
    if (source == 12)
        return texture(sampler2D(texChroma12, texSampler), uv);
    if (source == 13)
        return texture(sampler2D(texChroma13, texSampler), uv);
    if (source == 14)
        return texture(sampler2D(texChroma14, texSampler), uv);
    return texture(sampler2D(texChroma15, texSampler), uv);
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
    int outW = ub.uOutputSize.x;
    int outH = ub.uOutputSize.y;
    if (ub.uColumns <= 0 || ub.uRows <= 0 || outW <= 0 || outH <= 0) {
        fragColor = yuvToRgba8(16, 128, 128);
        return;
    }

    vec2 uv = clamp(vUv, vec2(0.0), vec2(0.999999));
    int px = clamp(int(floor(uv.x * float(outW))), 0, outW - 1);
    int py = clamp(int(floor(uv.y * float(outH))), 0, outH - 1);
    int col = min(ub.uColumns - 1, (((px + 1) * ub.uColumns) - 1) / outW);
    int row = min(ub.uRows - 1, (((py + 1) * ub.uRows) - 1) / outH);
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

    ivec4 tile = ub.uTileRect[source];
    int dstW = tile.z;
    int dstH = tile.w;
    if (dstW <= 0 || dstH <= 0) {
        fragColor = yuvToRgba8(16, 128, 128);
        return;
    }

    int ix = clamp(px - tile.x, 0, dstW - 1);
    int iy = clamp(py - tile.y, 0, dstH - 1);
    vec2 tileUv = vec2((float(ix) + 0.5) / float(dstW), (float(iy) + 0.5) / float(dstH));
    vec4 yTexel = lumaSample(source, tileUv);
    vec4 uvTexel = chromaSample(source, tileUv);
    fragColor = yuvToRgba8(texelU8(yTexel, 0), texelU8(uvTexel, 0), texelU8(uvTexel, 1));
}
