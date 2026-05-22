/* color_target=0x032a4000 */
/* regs: 0x00000012 0x00000000 0x3f430700 0xffff0070 0x00000111 0x00000081 0x00400000 0x80000000 0x80000000 */

/* VSH */
#version 450

layout(binding = 0, std140) uniform VshUniforms {
vec4 c[192];
vec4 clipRange;
vec2 fogParam;
vec4 inlineValue[16];
vec3 lightInfiniteDirection[8];
vec3 lightInfiniteHalfVector[8];
vec3 lightLocalAttenuation[8];
vec3 lightLocalPosition[8];
vec4 ltc1[20];
vec4 ltctxa[26];
vec4 ltctxb[52];
float material_alpha;
float pointParams[8];
float specularPower;
vec2 surfaceSize;
};

#define fogPlane c[0x39]
#define texMat0 mat4(c[0x44], c[0x44+1], c[0x44+2], c[0x44+3])
#define texMat1 mat4(c[0x4c], c[0x4c+1], c[0x4c+2], c[0x4c+3])
#define texMat2 mat4(c[0x54], c[0x54+1], c[0x54+2], c[0x54+3])
#define texMat3 mat4(c[0x5c], c[0x5c+1], c[0x5c+2], c[0x5c+3])

#define FLOAT_MAX uintBitsToFloat(0x7F7FFFFFu)

vec4 oPos = vec4(0.0,0.0,0.0,1.0);
vec4 oD0 = vec4(0.0,0.0,0.0,1.0);
vec4 oD1 = vec4(0.0,0.0,0.0,1.0);
vec4 oB0 = vec4(0.0,0.0,0.0,1.0);
vec4 oB1 = vec4(0.0,0.0,0.0,1.0);
vec4 oPts = vec4(0.0,0.0,0.0,1.0);
vec4 oFog = vec4(0.0,0.0,0.0,1.0);
vec4 oT0 = vec4(0.0,0.0,0.0,1.0);
vec4 oT1 = vec4(0.0,0.0,0.0,1.0);
vec4 oT2 = vec4(0.0,0.0,0.0,1.0);
vec4 oT3 = vec4(0.0,0.0,0.0,1.0);

vec4 decompress_11_11_10(int cmp) {
    float x = float(bitfieldExtract(cmp, 0,  11)) / 1023.0;
    float y = float(bitfieldExtract(cmp, 11, 11)) / 1023.0;
    float z = float(bitfieldExtract(cmp, 22, 10)) / 511.0;
    return vec4(x, y, z, 1);
}

float clampAwayZeroInf(float t) {
  if (t > 0.0 || floatBitsToUint(t) == 0) {
    t = clamp(t, uintBitsToFloat(0x1F800000), uintBitsToFloat(0x5F800000));
  } else {
    t = clamp(t, uintBitsToFloat(0xDF800000), uintBitsToFloat(0x9F800000));
  }
  return t;
}

vec4 NaNToOne(vec4 src) {
  return mix(src, vec4(1.0), isnan(src));
}
vec4 NaNToValue(vec4 src, float replacement) {
  return mix(src, vec4(replacement), isnan(src));
}

vec2 roundScreenCoords(vec2 pos) {
  return trunc(pos * 16.0f) / 16.0f;
}
layout(location = 0) out vec4 vtxD0;
layout(location = 1) out vec4 vtxD1;
layout(location = 2) out vec4 vtxB0;
layout(location = 3) out vec4 vtxB1;
layout(location = 4) out float vtxFog;
layout(location = 5) out vec4 vtxT0;
layout(location = 6) out vec4 vtxT1;
layout(location = 7) out vec4 vtxT2;
layout(location = 8) out vec4 vtxT3;
layout(location = 9) flat out vec4 vtxPos0;
layout(location = 10) flat out vec4 vtxPos1;
layout(location = 11) flat out vec4 vtxPos2;
layout(location = 12) flat out float triMZ;

vec4 v0 = inlineValue[0];
vec4 v1 = inlineValue[1];
vec4 v2 = inlineValue[2];
vec4 v3 = inlineValue[3];
vec4 v4 = inlineValue[4];
vec4 v5 = inlineValue[5];
vec4 v6 = inlineValue[6];
vec4 v7 = inlineValue[7];
vec4 v8 = inlineValue[8];
vec4 v9 = inlineValue[9];
vec4 v10 = inlineValue[10];
vec4 v11 = inlineValue[11];
vec4 v12 = inlineValue[12];
vec4 v13 = inlineValue[13];
vec4 v14 = inlineValue[14];
vec4 v15 = inlineValue[15];


int A0 = 0;

vec4 R0 = vec4(0.0,0.0,0.0,0.0);
vec4 R1 = vec4(0.0,0.0,0.0,0.0);
vec4 R2 = vec4(0.0,0.0,0.0,0.0);
vec4 R3 = vec4(0.0,0.0,0.0,0.0);
vec4 R4 = vec4(0.0,0.0,0.0,0.0);
vec4 R5 = vec4(0.0,0.0,0.0,0.0);
vec4 R6 = vec4(0.0,0.0,0.0,0.0);
vec4 R7 = vec4(0.0,0.0,0.0,0.0);
vec4 R8 = vec4(0.0,0.0,0.0,0.0);
vec4 R9 = vec4(0.0,0.0,0.0,0.0);
vec4 R10 = vec4(0.0,0.0,0.0,0.0);
vec4 R11 = vec4(0.0,0.0,0.0,0.0);
#define R12 oPos

vec4 _temp_vec;
int _temp_addr;

/* Converts the input to vec4, pads with last component */
vec4 _in(float v) { return vec4(v); }
vec4 _in(vec2 v) { return v.xyyy; }
vec4 _in(vec3 v) { return v.xyzz; }
vec4 _in(vec4 v) { return v.xyzw; }

#define INFINITY (1.0 / 0.0)

#define MOV(dest, mask, src) dest.mask = _MOV(_in(src)).mask
vec4 _MOV(vec4 src)
{
  return src;
}

#define MUL(dest, mask, src0, src1) dest.mask = _MUL(_in(src0), _in(src1)).mask
vec4 _MUL(vec4 src0, vec4 src1)
{
  vec4 zero_components = sign(NaNToOne(src0)) * sign(NaNToOne(src1));
  vec4 ret = src0 * src1;
  if (zero_components.x == 0.0) { ret.x = 0.0; }
  if (zero_components.y == 0.0) { ret.y = 0.0; }
  if (zero_components.z == 0.0) { ret.z = 0.0; }
  if (zero_components.w == 0.0) { ret.w = 0.0; }
  return ret;
}

#define ADD(dest, mask, src0, src1) dest.mask = _ADD(_in(src0), _in(src1)).mask
vec4 _ADD(vec4 src0, vec4 src1)
{
  return src0 + src1;
}

#define MAD(dest, mask, src0, src1, src2) dest.mask = _MAD(_in(src0), _in(src1), _in(src2)).mask
vec4 _MAD(vec4 src0, vec4 src1, vec4 src2)
{
  return _MUL(src0, src1) + src2;
}

#define DP3(dest, mask, src0, src1) dest.mask = _DP3(_in(src0), _in(src1)).mask
vec4 _DP3(vec4 src0, vec4 src1)
{
  return vec4(dot(src0.xyz, src1.xyz));
}

#define DPH(dest, mask, src0, src1) dest.mask = _DPH(_in(src0), _in(src1)).mask
vec4 _DPH(vec4 src0, vec4 src1)
{
  return vec4(dot(vec4(src0.xyz, 1.0), src1));
}

#define DP4(dest, mask, src0, src1) dest.mask = _DP4(_in(src0), _in(src1)).mask
vec4 _DP4(vec4 src0, vec4 src1)
{
  return vec4(dot(src0, src1));
}

#define DST(dest, mask, src0, src1) dest.mask = _DST(_in(src0), _in(src1)).mask
vec4 _DST(vec4 src0, vec4 src1)
{
  return vec4(1.0,
              src0.y * src1.y,
              src0.z,
              src1.w);
}

#define MIN(dest, mask, src0, src1) dest.mask = _MIN(_in(src0), _in(src1)).mask
vec4 _MIN(vec4 src0, vec4 src1)
{
  return min(src0, src1);
}

#define MAX(dest, mask, src0, src1) dest.mask = _MAX(_in(src0), _in(src1)).mask
vec4 _MAX(vec4 src0, vec4 src1)
{
  return max(src0, src1);
}

#define SLT(dest, mask, src0, src1) dest.mask = _SLT(_in(src0), _in(src1)).mask
vec4 _SLT(vec4 src0, vec4 src1)
{
  return vec4(lessThan(src0, src1));
}

#define ARL(dest, src) dest = _ARL(_in(src).x)
int _ARL(float src)
{
  /* Xbox GPU does specify rounding, OpenGL doesn't; so we need a bias.
   * Example: We probably want to floor 16.99.. to 17, not 16.
   * Source of error (why we get 16.99.. instead of 17.0) is typically
   * vertex-attributes being normalized from a byte value to float:
   *   17 / 255 = 0.06666.. so is this 0.06667 (ceil) or 0.06666 (floor)?
   * Which value we get depends on the host GPU.
   * If we multiply these rounded values by 255 later, we get:
   *   17.00 (ARL result = 17) or 16.99 (ARL result = 16).
   * We assume the intend was to get 17, so we add our bias to fix it. */
  return int(floor(src + 0.001));
}

#define SGE(dest, mask, src0, src1) dest.mask = _SGE(_in(src0), _in(src1)).mask
vec4 _SGE(vec4 src0, vec4 src1)
{
  return vec4(greaterThanEqual(src0, src1));
}

#define RCP(dest, mask, src) dest.mask = _RCP(_in(src).x).mask
vec4 _RCP(float src)
{
  return vec4(1.0 / src);
}

#define RCC(dest, mask, src) dest.mask = _RCC(_in(src).x).mask
vec4 _RCC(float src)
{
  float t = clampAwayZeroInf(1.0 / src);
  return vec4(t);
}

#define RSQ(dest, mask, src) dest.mask = _RSQ(_in(src).x).mask
vec4 _RSQ(float src)
{
  if (src == 0.0) { return vec4(INFINITY); }
  if (isinf(src)) { return vec4(0.0); }
  return vec4(inversesqrt(abs(src)));
}

#define EXP(dest, mask, src) dest.mask = _EXP(_in(src).x).mask
vec4 _EXP(float src)
{
  vec4 result;
  result.x = exp2(floor(src));
  result.y = src - floor(src);
  result.z = exp2(src);
  result.w = 1.0;
  return result;
}

#define LOG(dest, mask, src) dest.mask = _LOG(_in(src).x).mask
vec4 _LOG(float src)
{
  float tmp = abs(src);
  if (tmp == 0.0) { return vec4(-INFINITY, 1.0f, -INFINITY, 1.0f); }
  vec4 result;
  result.x = floor(log2(tmp));
  result.y = tmp / exp2(floor(log2(tmp)));
  result.z = log2(tmp);
  result.w = 1.0;
  return result;
}

#define LIT(dest, mask, src) dest.mask = _LIT(_in(src)).mask
vec4 _LIT(vec4 src)
{
  vec4 s = src;
  float epsilon = 1.0 / 256.0;
  s.w = clamp(s.w, -(128.0 - epsilon), 128.0 - epsilon);
  s.x = max(s.x, 0.0);
  s.y = max(s.y, 0.0);
  vec4 t = vec4(1.0, 0.0, 0.0, 1.0);
  t.y = s.x;
  t.z = (s.x > 0.0) ? exp2(s.w * log2(s.y)) : 0.0;
  return t;
}
void main() {
  if (surfaceSize.x < 0.0) {
    vec4 mtl_keepalive = c[0] + clipRange +
        vec4(fogParam, 0.0, 0.0) + inlineValue[0] +
        vec4(lightInfiniteDirection[0], 0.0) +
        vec4(lightInfiniteHalfVector[0], 0.0) +
        vec4(lightLocalAttenuation[0], 0.0) +
        vec4(lightLocalPosition[0], 0.0) +
        ltc1[0] + ltctxa[0] + ltctxb[0] +
        vec4(material_alpha + pointParams[0] + specularPower,
             surfaceSize.x, surfaceSize.y, 0.0);
    oPos += mtl_keepalive;
  }
  /* Slot 0: 0x00000000 0x0020001B 0x0836106C 0x2F100FF8 */
    MOV(R1,xyzw, v0);

  /* Slot 1: 0x00000000 0x0420061B 0x083613FC 0x5011F818 */
    MOV(oD0,xyzw, v3);
  RCP(R1,w, R1.w);

  /* Slot 2: 0x00000000 0x002008FF 0x0836106C 0x2070F828 */
    MOV(oFog,xyzw, v4.w);

  /* Slot 3: 0x00000000 0x0240081B 0x1436186C 0x2F20F824 */
    MUL(_temp_vec,xyzw, R1, c[0]);
  MOV(oD1,xyzw, v4);
  R2.xyzw = _temp_vec.xyzw;

  /* Slot 4: 0x00000000 0x0060201B 0x2436106C 0x3070F800 */
    ADD(oPos,xyzw, R2, c[1]);

  /* Slot 5: 0x00000000 0x00200200 0x0836106C 0x2070F830 */
    MOV(oPts,xyzw, v1.x);

  /* Slot 6: 0x00000000 0x00200E1B 0x0836106C 0x2070F838 */
    MOV(oB0,xyzw, v7);

  /* Slot 7: 0x00000000 0x0020101B 0x0836106C 0x2070F840 */
    MOV(oB1,xyzw, v8);

  /* Slot 8: 0x00000000 0x0020121B 0x0836106C 0x2070F848 */
    MOV(oT0,xyzw, v9);

  /* Slot 9: 0x00000000 0x0020141B 0x0836106C 0x2070F850 */
    MOV(oT1,xyzw, v10);

  /* Slot 10: 0x00000000 0x0020161B 0x0836106C 0x2070F858 */
    MOV(oT2,xyzw, v11);

  /* Slot 11: 0x00000000 0x0020181B 0x0836106C 0x2070F861 */
    MOV(oT3,xyzw, v12);

  oPos.xy = roundScreenCoords(oPos.xy);
  oPos.w = clampAwayZeroInf(oPos.w);
  vec4 vtxPos = oPos;
  oPos.xy = (2.0f * oPos.xy - surfaceSize) / surfaceSize;
  oPos.z = oPos.z / clipRange.y;
  oPos.xyz *= oPos.w;
  oPts.x = 1.000000 * 1;
  oFog = vec4(1.0);

  vtxD0 = clamp(NaNToOne(oD0), 0.0, 1.0);
  vtxB0 = clamp(NaNToOne(oB0), 0.0, 1.0);
  vtxFog = oFog.x;
  vtxT0 = oT0;
  vtxT1 = oT1;
  vtxT2 = oT2;
  vtxT3 = oT3;
  vtxPos0 = vtxPos;
  vtxPos1 = vtxPos;
  vtxPos2 = vtxPos;
  triMZ = 0.0;
  gl_PointSize = oPts.x;
  vtxD1 = vec4(0.0, 0.0, 0.0, 1.0);
  vtxB1 = vec4(0.0, 0.0, 0.0, 1.0);
  gl_Position = oPos;
}


/* PSH */
#version 450

layout(location = 0) in vec4 vtxD0;
layout(location = 1) in vec4 vtxD1;
layout(location = 2) in vec4 vtxB0;
layout(location = 3) in vec4 vtxB1;
layout(location = 4) in float vtxFog;
layout(location = 5) in vec4 vtxT0;
layout(location = 6) in vec4 vtxT1;
layout(location = 7) in vec4 vtxT2;
layout(location = 8) in vec4 vtxT3;
layout(location = 9) flat in vec4 vtxPos0;
layout(location = 10) flat in vec4 vtxPos1;
layout(location = 11) flat in vec4 vtxPos2;
layout(location = 12) flat in float triMZ;
layout(location = 0) out vec4 fragColor;
layout(binding = 1, std140) uniform PshUniforms {
int alphaRef;
mat2 bumpMat[4];
float bumpOffset[4];
float bumpScale[4];
vec4 clipRange;
ivec4 clipRegion[8];
uint colorKey[4];
uint colorKeyMask[4];
vec4 consts[18];
float depthFactor;
float depthOffset;
vec4 fogColor;
ivec2 surfaceScale;
float texScale[4];
#define c0_0 consts[0]
#define c1_0 consts[1]
#define c0_1 consts[2]
#define c1_1 consts[3]
#define c0_2 consts[4]
#define c1_2 consts[5]
#define c0_3 consts[6]
#define c1_3 consts[7]
#define c0_4 consts[8]
#define c1_4 consts[9]
#define c0_5 consts[10]
#define c1_5 consts[11]
#define c0_6 consts[12]
#define c1_6 consts[13]
#define c0_7 consts[14]
#define c1_7 consts[15]
#define c0_8 consts[16]
#define c1_8 consts[17]
};
float sign1(float x) {
    x *= 255.0;
    return (x-128.0)/127.0;
}
float sign2(float x) {
    x *= 255.0;
    if (x >= 128.0) return (x-255.5)/127.5;
               else return (x+0.5)/127.5;
}
float sign3(float x) {
    x *= 255.0;
    if (x >= 128.0) return (x-256.0)/127.0;
               else return (x)/127.0;
}
float sign3_to_0_to_1(float x) {
    if (x >= 0) return x/2;
           else return 1+x/2;
}
vec3 dotmap_zero_to_one(vec4 col) {
    return col.rgb;
}
vec3 dotmap_minus1_to_1_d3d(vec4 col) {
    return vec3(sign1(col.r),sign1(col.g),sign1(col.b));
}
vec3 dotmap_minus1_to_1_gl(vec4 col) {
    return vec3(sign2(col.r),sign2(col.g),sign2(col.b));
}
vec3 dotmap_minus1_to_1(vec4 col) {
    return vec3(sign3(col.r),sign3(col.g),sign3(col.b));
}
vec3 dotmap_hilo_1(vec4 col) {
    uint hi_i = uint(col.a * float(0xff)) << 8
              | uint(col.r * float(0xff));
    uint lo_i = uint(col.g * float(0xff)) << 8
              | uint(col.b * float(0xff));
    float hi_f = float(hi_i) / float(0xffff);
    float lo_f = float(lo_i) / float(0xffff);
    return vec3(hi_f, lo_f, 1.0);
}
vec3 dotmap_hilo_hemisphere_d3d(vec4 col) {
    return col.rgb;
}
vec3 dotmap_hilo_hemisphere_gl(vec4 col) {
    return col.rgb;
}
vec3 dotmap_hilo_hemisphere(vec4 col) {
    return col.rgb;
}
float kahan_det(vec2 a, vec2 b) {
    precise float cd = a.y*b.x;
    precise float err = fma(-a.y, b.x, cd);
    precise float res = fma(a.x, b.y, -cd) + err;
    return res;
}
float area(vec2 a, vec2 b, vec2 c) {
    return kahan_det(b - a, c - a);
}
const float[9] gaussian3x3 = float[9](
    1.0/16.0, 2.0/16.0, 1.0/16.0,
    2.0/16.0, 4.0/16.0, 2.0/16.0,
    1.0/16.0, 2.0/16.0, 1.0/16.0);
const vec2[9] convolution3x3 = vec2[9](
    vec2(-1.0,-1.0),vec2(0.0,-1.0),vec2(1.0,-1.0),
    vec2(-1.0, 0.0),vec2(0.0, 0.0),vec2(1.0, 0.0),
    vec2(-1.0, 1.0),vec2(0.0, 1.0),vec2(1.0, 1.0));
vec2 remapCubeTo2D(vec3 texCoord) {
    vec2 uv;
    vec3 absTexCoord = abs(texCoord);
    if (absTexCoord.x > absTexCoord.y && absTexCoord.x > absTexCoord.z) {
        if (texCoord.x > 0.0) {
            // +X: Right
            uv = vec2(-texCoord.z, texCoord.y);
        } else {
            // -X: Left
            uv = vec2(texCoord.z, texCoord.y);
        }
        uv /= absTexCoord.x;
    }
    else if (absTexCoord.y > absTexCoord.x && absTexCoord.y > absTexCoord.z) {
        if (texCoord.y > 0.0) {
            // +Y: Top
            uv = vec2(texCoord.x, -texCoord.z);
        } else {
            // -Y: Bottom
            uv = vec2(texCoord.x, texCoord.z);
        }
        uv /= absTexCoord.y;
    }
    else {
        if (texCoord.z > 0.0) {
            // +Z: Front
            uv = vec2(texCoord.x, texCoord.y);
        } else {
            // -Z: Back
            uv = vec2(-texCoord.x, texCoord.y);
        }
        uv /= absTexCoord.z;
    }
    return uv;
}

vec3 remap2DToCube(vec3 texCoord2DProjective) {
    vec2 st = (texCoord2DProjective.xy / texCoord2DProjective.z);    return normalize(vec3(1.0, st.y, -st.x));}
layout(binding = 2) uniform sampler2D texSamp0;
vec2 norm0(vec2 coord) {
    return coord / (textureSize(texSamp0, 0) / texScale[0]);
}
vec3 norm0(vec3 coord) {
    return vec3(norm0(coord.xy), coord.z);
}
vec4 norm0(vec4 coord) {
    return vec4(norm0(coord.xy), 0, coord.w);
}
void main() {
/*  Window-clip (Inclusive) */
bool clipContained = false;
vec2 coord = gl_FragCoord.xy - 0.5;
for (int i = 0; i < 8; i++) {
  bool outside = any(bvec4(
      lessThan(coord, vec2(clipRegion[i].xy)),
      greaterThanEqual(coord, vec2(clipRegion[i].zw))));
  if (!outside) {
    clipContained = true;
    break;
  }
}
if (!clipContained) {
  discard;
}
precise float zvalue = gl_FragCoord.z * clipRange.y;
float nativeTriMZ = max(abs(dFdx(zvalue) * float(surfaceScale.x)),
                       abs(dFdy(zvalue) * float(surfaceScale.y)));
zvalue += depthOffset;
zvalue += depthFactor*nativeTriMZ;
if (zvalue < clipRange.z || clipRange.w < zvalue) {
  discard;
}
vec4 pD0 = vtxD0;
vec4 pD1 = vtxD1;
vec4 pB0 = vtxB0;
vec4 pB1 = vtxB1;
vec4 pFog = vec4(fogColor.rgb, clamp(vtxFog, 0.0, 1.0));
vec4 pT0 = vtxT0;
vec4 pT1 = vtxT1;
vec4 pT2 = vtxT2;
vec4 pT3 = vtxT3;

vec4 v0 = pD0;
vec4 v1 = pD1;
vec4 ab;
vec4 cd;
vec4 mux_sum;
float dot0 = 0.0;
float dot1 = 0.0;
float dot2 = 0.0;
float dot3 = 0.0;
vec4 t0 = textureProj(texSamp0, norm0(pT0.xyw));
vec4 t1 = vec4(0.0, 0.0, 0.0, 1.0); /* PS_TEXTUREMODES_NONE */
vec4 t2 = vec4(0.0, 0.0, 0.0, 1.0); /* PS_TEXTUREMODES_NONE */
vec4 t3 = vec4(0.0, 0.0, 0.0, 1.0); /* PS_TEXTUREMODES_NONE */
vec4 r0 = vec4(0);
r0.a = t0.a;
// Stage 0
mux_sum.rgb = clamp(vec3(((max(t0.rgb, 0.0) * (1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0))) + (max(vec4(0.0).rgb, 0.0) * max(vec4(0.0).rgb, 0.0)))), -1.0, 1.0);
mux_sum.a = clamp((((max(v0.a, 0.0) * (1.0 - clamp(vec4(0.0).b, 0.0, 1.0))) + (max(vec4(0.0).b, 0.0) * max(vec4(0.0).b, 0.0)))), -1.0, 1.0);
r0.rgb = mux_sum.rgb;
r0.a = mux_sum.a;
// Final Combiner
fragColor.rgb = max(r0.rgb, 0.0) + mix(vec3(max(vec4(0.0).rgb, 0.0)), vec3(max(vec4(0.0).rgb, 0.0)), vec3(max(vec4(0.0).rgb, 0.0)));
fragColor.a = max(r0.a, 0.0);
gl_FragDepth = uintBitsToFloat(floatBitsToUint(floor(zvalue) / 16777216.0) + 1u);
}

