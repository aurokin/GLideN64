#version 450

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inTexCoord0;
layout(location = 2) in vec2 inTexCoord1;
layout(location = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outColor1;

layout(set = 0, binding = 0) uniform sampler2D uTextures[8];
layout(set = 0, binding = 0) uniform usampler2D uTexturesUint[8];

layout(push_constant) uniform DrawPushConstants {
    uint flags;
    uint texrectAlphaTest;
    uint texrectFilterMode;
    uint reserved0;
    float texrectTextureWidth;
    float texrectTextureHeight;
    float gammaLevel;
    float reserved1;
    float textColorR;
    float textColorG;
    float textColorB;
    float textColorA;
    float fogColorR;
    float fogColorG;
    float fogColorB;
    float fogColorA;
} pushData;

const uint kTexture0 = 1u << 0;
const uint kTexture1 = 1u << 1;
const uint kShade = 1u << 2;
const uint kSpecialTexrectDraw = 1u << 3;
const uint kSpecialDepthFromTexture1 = 1u << 4;
const uint kSpecialGammaCorrection = 1u << 5;
const uint kSpecialFXAA = 1u << 6;
const uint kSpecialTextDraw = 1u << 7;
const uint kSpecialDepthFog = 1u << 8;
const uint kSpecialStrictBlendMux = 1u << 9;
const uint kCombinerAddConstant = 1u << 10;
const uint kShadeRGBOnly = 1u << 11;

const vec4 kTexrectTestColor = vec4(4.0 / 255.0, 2.0 / 255.0, 1.0 / 255.0, 0.0);

bool isTestColor(vec4 c)
{
    return all(equal(c, kTexrectTestColor));
}

vec4 patchTestColor(vec4 sampleColor, vec4 fallbackColor)
{
    return isTestColor(sampleColor) ? fallbackColor : sampleColor;
}

vec4 sampleTexrectSpecial(vec2 uv)
{
    vec4 base = texture(uTextures[0], uv);
    if (isTestColor(base)) {
        discard;
    }
    if (pushData.texrectAlphaTest != 0u && !(base.a > 0.0)) {
        discard;
    }

    vec2 texSize = vec2(max(pushData.texrectTextureWidth, 1.0), max(pushData.texrectTextureHeight, 1.0));
    vec2 offset = fract(uv * texSize - vec2(0.5));
    offset -= step(1.0, offset.x + offset.y);

    if (pushData.texrectFilterMode == 1u) {
        vec4 c0 = texture(uTextures[0], uv - offset / texSize);
        vec4 c1 = texture(uTextures[0], uv - vec2(offset.x - sign(offset.x), offset.y) / texSize);
        vec4 c2 = texture(uTextures[0], uv - vec2(offset.x, offset.y - sign(offset.y)) / texSize);
        c0 = patchTestColor(c0, base);
        c1 = patchTestColor(c1, base);
        c2 = patchTestColor(c2, base);
        return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);
    }

    if (pushData.texrectFilterMode == 2u) {
        vec4 p0q0 = texture(uTextures[0], uv - offset / texSize);
        vec4 p1q0 = texture(uTextures[0], uv - vec2(offset.x - sign(offset.x), offset.y) / texSize);
        vec4 p0q1 = texture(uTextures[0], uv - vec2(offset.x, offset.y - sign(offset.y)) / texSize);
        vec4 p1q1 = texture(uTextures[0], uv - vec2(offset.x - sign(offset.x), offset.y - sign(offset.y)) / texSize);
        p0q0 = patchTestColor(p0q0, base);
        p1q0 = patchTestColor(p1q0, base);
        p0q1 = patchTestColor(p0q1, base);
        p1q1 = patchTestColor(p1q1, base);
        vec2 interpolationFactor = abs(offset);
        vec4 row0 = mix(p0q0, p1q0, interpolationFactor.x);
        vec4 row1 = mix(p0q1, p1q1, interpolationFactor.x);
        return mix(row0, row1, interpolationFactor.y);
    }

    return base;
}

vec4 sampleFXAA(vec2 uv)
{
    vec2 texel = 1.0 / vec2(textureSize(uTextures[0], 0));
    vec3 rgbNW = texture(uTextures[0], uv + vec2(-1.0, -1.0) * texel).rgb;
    vec3 rgbNE = texture(uTextures[0], uv + vec2(1.0, -1.0) * texel).rgb;
    vec3 rgbSW = texture(uTextures[0], uv + vec2(-1.0, 1.0) * texel).rgb;
    vec3 rgbSE = texture(uTextures[0], uv + vec2(1.0, 1.0) * texel).rgb;
    vec4 rgbaM = texture(uTextures[0], uv);
    vec3 rgbM = rgbaM.rgb;

    vec3 lumaCoeff = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, lumaCoeff);
    float lumaNE = dot(rgbNE, lumaCoeff);
    float lumaSW = dot(rgbSW, lumaCoeff);
    float lumaSE = dot(rgbSE, lumaCoeff);
    float lumaM = dot(rgbM, lumaCoeff);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    const float FXAA_REDUCE_MIN = 1.0 / 128.0;
    const float FXAA_REDUCE_MUL = 1.0 / 8.0;
    const float FXAA_SPAN_MAX = 8.0;

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * texel;

    vec3 rgbA = 0.5 * (
        texture(uTextures[0], uv + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(uTextures[0], uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(uTextures[0], uv + dir * -0.5).rgb +
        texture(uTextures[0], uv + dir * 0.5).rgb);

    float lumaB = dot(rgbB, lumaCoeff);
    vec3 finalRgb = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return vec4(finalRgb, rgbaM.a);
}

vec4 sampleDepthFogSpecial()
{
    ivec2 depthSize = textureSize(uTextures[0], 0);
    ivec2 depthCoord = clamp(ivec2(gl_FragCoord.xy), ivec2(0), depthSize - ivec2(1));
    float bufZ = texelFetch(uTextures[0], depthCoord, 0).r;
    int iZ = bufZ > 0.999 ? 262143 : int(floor(bufZ * 262143.0));
    int y0 = clamp(iZ / 512, 0, 511);
    int x0 = iZ - (512 * y0);
    uint iN64z = texelFetch(uTexturesUint[3], ivec2(x0, y0), 0).r;
    float n64z = clamp(float(iN64z) / 65532.0, 0.0, 1.0);
    int index = min(255, int(n64z * 255.0));
    uint iAlpha = texelFetch(uTexturesUint[4], ivec2(index, 0), 0).r;
    float alpha = float((iAlpha >> 8u) & 0xFFu) / 255.0;
    return vec4(pushData.fogColorR, pushData.fogColorG, pushData.fogColorB, alpha);
}

uvec4 unpackBlendMux(uint packed)
{
    return uvec4(
        packed & 0x3u,
        (packed >> 2u) & 0x3u,
        (packed >> 4u) & 0x3u,
        (packed >> 6u) & 0x3u);
}

float selectScalar4(vec4 values, uint index)
{
    if (index == 0u) return values.x;
    if (index == 1u) return values.y;
    if (index == 2u) return values.z;
    return values.w;
}

vec4 selectVector4(vec4 v0, vec4 v1, vec4 v2, vec4 v3, uint index)
{
    if (index == 0u) return v0;
    if (index == 1u) return v1;
    if (index == 2u) return v2;
    return v3;
}

void applyStrictBlendMux(in vec4 shadeColor, in vec4 baseColor, out vec4 outPrimary, out vec4 outSecondary)
{
    uvec4 blendMux1 = unpackBlendMux(pushData.texrectAlphaTest);
    uvec4 blendMux2 = unpackBlendMux(pushData.texrectFilterMode);
    uint blendParams = pushData.reserved0;
    bool forceBlendCycle1 = (blendParams & 0x1u) != 0u;
    bool forceBlendCycle2 = (blendParams & 0x2u) != 0u;
    bool twoCycle = (blendParams & 0x4u) != 0u;
    uint blendAlphaMode = (blendParams >> 4u) & 0x3u;
    uint cvgDest = (blendParams >> 6u) & 0x3u;
    vec4 blendColor = vec4(pushData.textColorR, pushData.textColorG, pushData.textColorB, pushData.textColorA);
    vec4 fogColor = vec4(pushData.fogColorR, pushData.fogColorG, pushData.fogColorB, pushData.fogColorA);
    vec4 lastFragColor = vec4(0.0);
    float lastFragAlpha = 1.0;

    vec4 muxA = vec4(clamp(baseColor.a, 0.0, 1.0), fogColor.a, shadeColor.a, 0.0);
    vec4 muxB = vec4(0.0, lastFragAlpha, 1.0, 0.0);
    vec4 muxF = vec4(0.0, 1.0, 0.0, 0.0);

    vec4 muxp = selectVector4(baseColor, lastFragColor, blendColor, fogColor, blendMux1.x);
    vec4 muxm = selectVector4(baseColor, lastFragColor, blendColor, fogColor, blendMux1.z);
    float muxa = selectScalar4(muxA, blendMux1.y);
    muxB.x = 1.0 - muxa;
    float muxb = selectScalar4(muxB, blendMux1.w);
    float muxaf = selectScalar4(muxF, blendMux1.x);
    float muxbf = selectScalar4(muxF, blendMux1.z);

    vec4 srcColor1;
    float dstFactor1;
    if (forceBlendCycle1) {
        srcColor1 = clamp(muxp * muxa + muxm * muxb, 0.0, 1.0);
        dstFactor1 = clamp(muxaf * muxa + muxbf * muxb, 0.0, 1.0);
    } else {
        srcColor1 = muxp;
        dstFactor1 = muxaf;
    }

    vec4 finalSrcColor = srcColor1;
    float finalDstFactor = dstFactor1;

    if (twoCycle) {
        vec4 muxF2 = vec4(dstFactor1, 1.0, 0.0, 0.0);
        vec4 muxp2 = selectVector4(srcColor1, lastFragColor, blendColor, fogColor, blendMux2.x);
        vec4 muxm2 = selectVector4(srcColor1, lastFragColor, blendColor, fogColor, blendMux2.z);
        float muxa2 = selectScalar4(muxA, blendMux2.y);
        vec4 muxB2 = vec4(1.0 - muxa2, lastFragAlpha, 1.0, 0.0);
        float muxb2 = selectScalar4(muxB2, blendMux2.w);
        float muxaf2 = selectScalar4(muxF2, blendMux2.x);
        float muxbf2 = selectScalar4(muxF2, blendMux2.z);
        if (forceBlendCycle2) {
            finalSrcColor = clamp(muxp2 * muxa2 + muxm2 * muxb2, 0.0, 1.0);
            finalDstFactor = clamp(muxaf2 * muxa2 + muxbf2 * muxb2, 0.0, 1.0);
        } else {
            finalSrcColor = muxp2;
            finalDstFactor = muxaf2;
        }
    }

    outPrimary = baseColor;
    outSecondary = vec4(finalDstFactor);
    if (blendAlphaMode != 2u) {
        vec4 dstFactorAlpha = vec4(1.0, 1.0, 0.0, 1.0);
        if (blendAlphaMode == 0u) {
            dstFactorAlpha.x = 0.0;
        }
        outSecondary.a = dstFactorAlpha[min(cvgDest, 3u)];
    } else {
        outSecondary.a = finalDstFactor;
    }
}

void main()
{
    vec4 color = vec4(1.0);
    vec4 secondaryColor = vec4(0.0);
    bool hasColor = false;

    if ((pushData.flags & kSpecialDepthFog) != 0u) {
        color = sampleDepthFogSpecial();
        hasColor = true;
    } else if ((pushData.flags & kSpecialTextDraw) != 0u) {
        vec4 texColor = texture(uTextures[0], inTexCoord0);
        vec4 textColor = vec4(pushData.textColorR, pushData.textColorG, pushData.textColorB, pushData.textColorA);
        color = pow(texColor.r, 1.0 / 1.8) * textColor;
        hasColor = true;
    } else if ((pushData.flags & kSpecialFXAA) != 0u) {
        color = sampleFXAA(inTexCoord0);
        hasColor = true;
    }

    if (!hasColor && (pushData.flags & kTexture0) != 0u) {
        if ((pushData.flags & kSpecialTexrectDraw) != 0u) {
            color = sampleTexrectSpecial(inTexCoord0);
        } else {
            color = texture(uTextures[0], inTexCoord0);
        }
        hasColor = true;
    }

    if ((pushData.flags & kTexture1) != 0u) {
        vec4 tex1 = texture(uTextures[1], inTexCoord1);
        if ((pushData.flags & kSpecialDepthFromTexture1) != 0u) {
            gl_FragDepth = clamp(tex1.r, 0.0, 1.0);
        } else {
            if (hasColor) {
                color *= tex1;
            } else {
                color = tex1;
                hasColor = true;
            }
        }
    }

    if ((pushData.flags & kShade) != 0u) {
        bool shadeRgbOnly = (pushData.flags & kShadeRGBOnly) != 0u;
        if (hasColor) {
            if (shadeRgbOnly) {
                color.rgb *= inColor.rgb;
            } else {
                color *= inColor;
            }
        } else {
            if (shadeRgbOnly) {
                color = vec4(inColor.rgb, 1.0);
            } else {
                color = inColor;
            }
            hasColor = true;
        }
    }

    if ((pushData.flags & kCombinerAddConstant) != 0u) {
        vec4 addColor = vec4(pushData.fogColorR, pushData.fogColorG, pushData.fogColorB, pushData.fogColorA);
        color = clamp(color + addColor, 0.0, 1.0);
    }

    if ((pushData.flags & kSpecialStrictBlendMux) != 0u) {
        vec4 strictPrimary = color;
        vec4 strictSecondary = vec4(0.0);
        applyStrictBlendMux(inColor, color, strictPrimary, strictSecondary);
        color = strictPrimary;
        secondaryColor = strictSecondary;
    }

    if ((pushData.flags & kSpecialGammaCorrection) != 0u) {
        float gammaLevel = max(pushData.gammaLevel, 0.001);
        color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(1.0 / gammaLevel));
    }

    if (!hasColor) {
        color = inColor;
    }

    outColor = color;
    outColor1 = secondaryColor;
}
