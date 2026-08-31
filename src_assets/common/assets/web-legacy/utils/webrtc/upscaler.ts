/**
 * WebGL2 video upscaler: AMD FidelityFX Super Resolution 1.0 (EASU + RCAS).
 *
 * Ported from the reference ffx_fsr1.h / ffx_a.h (GPUOpen-Effects/FidelityFX-FSR,
 * MIT). Deviations from the reference, both deliberate:
 *
 * - WebGL2 is GLSL ES 3.00, which has no textureGather (that needs ES 3.1), so the
 *   12-tap footprint is fetched with texelFetch per tap instead of four gather4s.
 *   Same taps, same math, more fetch instructions.
 * - The reference packs its constants into uint4s to ship them through constant
 *   buffers; here they stay plain float uniforms.
 *
 * Two passes per frame: EASU scales the decoded frame to the displayed size into an
 * intermediate texture, RCAS sharpens that into the canvas. The video element keeps
 * playing underneath (it stays the decode surface and the audio sink) — the caller
 * hides it visually while the canvas is active.
 *
 * WebGL2 being unavailable, a shader failing to compile, or a lost context all
 * degrade to "no upscaler": attach() returns null or calls onFailure, and the plain
 * video element is what the guest sees. Never worse than the status quo.
 */

interface UpscalerOptions {
  /** RCAS sharpness in stops: 0 = maximum sharpening, higher = softer. */
  sharpnessStops?: number;
  /** Cap on devicePixelRatio, to bound the EASU output size on 3x phone screens. */
  maxDevicePixelRatio?: number;
  /** Called once if the pipeline dies at runtime (context lost). */
  onFailure?: () => void;
}

export interface VideoUpscaler {
  detach(): void;
}

const VERT = `#version 300 es
void main() {
  // Fullscreen triangle from gl_VertexID; no buffers needed.
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

// ffx_a.h approximations, verbatim constants.
const APRX = `
float APrxLoRcpF1(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float APrxMedRcpF1(float a) {
  float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a));
  return b * (-b * a + 2.0);
}
float APrxLoRsqF1(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1u)); }
`;

const EASU_FRAG = `#version 300 es
precision highp float;
uniform sampler2D uSource;
uniform vec2 uScale;   // FsrEasuCon con0.xy: input size / output size
uniform vec2 uBias;    // FsrEasuCon con0.zw: 0.5*scale - 0.5
uniform ivec2 uSourceMax; // source size - 1, for tap clamping
out vec4 outColor;
${APRX}
vec3 tap(ivec2 c) {
  return texelFetch(uSource, clamp(c, ivec2(0), uSourceMax), 0).rgb;
}
// FsrEasuTapF: accumulate one tap of the rotated, anisotropic lanczos2 approximation.
void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
  vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x);
  v *= len;
  float d2 = min(dot(v, v), clp);
  float wB = 0.4 * d2 - 1.0;
  float wA = lob * d2 - 1.0;
  wB *= wB;
  wA *= wA;
  wB = 1.5625 * wB - 0.5625;
  float w = wB * wA;
  aC += c * w;
  aW += w;
}
// FsrEasuSetF: accumulate gradient direction and length from a plus-pattern
// (a above, b left, c centre, d right, e below), weighted bilinearly.
void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
  float dc = lD - lC;
  float cb = lC - lB;
  float lenX = max(abs(dc), abs(cb));
  lenX = APrxLoRcpF1(lenX);
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
  lenX *= lenX;
  len += lenX * w;
  float ec = lE - lC;
  float ca = lC - lA;
  float lenY = max(abs(ec), abs(ca));
  lenY = APrxLoRcpF1(lenY);
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
  lenY *= lenY;
  len += lenY * w;
}
void main() {
  // FsrEasuF. Output pixel to source-space position of 'f'.
  vec2 pp = floor(gl_FragCoord.xy) * uScale + uBias;
  vec2 fp = floor(pp);
  pp -= fp;
  ivec2 base = ivec2(fp);
  // 12-tap footprint:
  //    b c
  //  e f g h
  //  i j k l
  //    n o
  vec3 cb = tap(base + ivec2(0, -1));
  vec3 cc = tap(base + ivec2(1, -1));
  vec3 ce = tap(base + ivec2(-1, 0));
  vec3 cf = tap(base);
  vec3 cg = tap(base + ivec2(1, 0));
  vec3 ch = tap(base + ivec2(2, 0));
  vec3 ci = tap(base + ivec2(-1, 1));
  vec3 cj = tap(base + ivec2(0, 1));
  vec3 ck = tap(base + ivec2(1, 1));
  vec3 cl = tap(base + ivec2(2, 1));
  vec3 cn = tap(base + ivec2(0, 2));
  vec3 co = tap(base + ivec2(1, 2));
  // Approximate luma times 2.
  #define L(c) (c.b * 0.5 + (c.r * 0.5 + c.g))
  float bL = L(cb); float cL = L(cc); float eL = L(ce); float fL = L(cf);
  float gL = L(cg); float hL = L(ch); float iL = L(ci); float jL = L(cj);
  float kL = L(ck); float lL = L(cl); float nL = L(cn); float oL = L(co);
  #undef L
  vec2 dir = vec2(0.0);
  float len = 0.0;
  easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
  easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
  easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
  // Normalize direction, with a cleanup close to zero.
  vec2 dir2 = dir * dir;
  float dirR = dir2.x + dir2.y;
  bool zro = dirR < (1.0 / 32768.0);
  dirR = APrxLoRsqF1(dirR);
  dirR = zro ? 1.0 : dirR;
  dir.x = zro ? 1.0 : dir.x;
  dir *= dirR;
  len = len * 0.5;
  len *= len;
  float stretch = dot(dir, dir) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
  float clp = APrxLoRcpF1(lob);
  // Dering clamp comes from the 4 nearest taps.
  vec3 min4 = min(min(cf, cg), min(cj, ck));
  vec3 max4 = max(max(cf, cg), max(cj, ck));
  vec3 aC = vec3(0.0);
  float aW = 0.0;
  easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, cb);
  easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cc);
  easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, ci);
  easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, cj);
  easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, cf);
  easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, ce);
  easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, ck);
  easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, cl);
  easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, ch);
  easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, cg);
  easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, co);
  easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, cn);
  vec3 pix = min(max4, max(min4, aC * (1.0 / aW)));
  outColor = vec4(pix, 1.0);
}`;

const RCAS_FRAG = `#version 300 es
precision highp float;
uniform sampler2D uSource;
uniform float uSharpness; // exp2(-stops), FsrRcasCon
uniform ivec2 uOrigin;    // viewport origin on the canvas
uniform ivec2 uSourceMax;
out vec4 outColor;
${APRX}
vec3 load(ivec2 c) {
  return texelFetch(uSource, clamp(c, ivec2(0), uSourceMax), 0).rgb;
}
void main() {
  // FsrRcasF over the minimal plus-pattern neighborhood.
  ivec2 sp = ivec2(gl_FragCoord.xy) - uOrigin;
  vec3 b = load(sp + ivec2(0, -1));
  vec3 d = load(sp + ivec2(-1, 0));
  vec3 e = load(sp);
  vec3 f = load(sp + ivec2(1, 0));
  vec3 h = load(sp + ivec2(0, 1));
  vec3 mn4 = min(min(b, d), min(f, h));
  vec3 mx4 = max(max(b, d), max(f, h));
  vec2 peakC = vec2(1.0, -4.0);
  vec3 hitMin = min(mn4, e) / (4.0 * mx4);
  vec3 hitMax = (peakC.x - max(mx4, e)) / (4.0 * mn4 + peakC.y);
  vec3 lobeRGB = max(-hitMin, hitMax);
  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * uSharpness;
  float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
  vec3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
  outColor = vec4(pix, 1.0);
}`;

function compile(gl: WebGL2RenderingContext, type: number, source: string): WebGLShader | null {
  const shader = gl.createShader(type);
  if (!shader) return null;
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    gl.deleteShader(shader);
    return null;
  }
  return shader;
}

function link(gl: WebGL2RenderingContext, frag: string): WebGLProgram | null {
  const vs = compile(gl, gl.VERTEX_SHADER, VERT);
  const fs = compile(gl, gl.FRAGMENT_SHADER, frag);
  if (!vs || !fs) return null;
  const program = gl.createProgram();
  if (!program) return null;
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  gl.deleteShader(vs);
  gl.deleteShader(fs);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    gl.deleteProgram(program);
    return null;
  }
  return program;
}

export function attachVideoUpscaler(
  video: HTMLVideoElement,
  canvas: HTMLCanvasElement,
  options: UpscalerOptions = {},
): VideoUpscaler | null {
  const gl = canvas.getContext('webgl2', {
    alpha: false,
    antialias: false,
    depth: false,
    stencil: false,
    powerPreference: 'high-performance',
  });
  if (!gl) return null;

  const easu = link(gl, EASU_FRAG);
  const rcas = link(gl, RCAS_FRAG);
  const sourceTex = gl.createTexture();
  const midTex = gl.createTexture();
  const fbo = gl.createFramebuffer();
  const vao = gl.createVertexArray();
  if (!easu || !rcas || !sourceTex || !midTex || !fbo || !vao) return null;

  const uEasu = {
    scale: gl.getUniformLocation(easu, 'uScale'),
    bias: gl.getUniformLocation(easu, 'uBias'),
    sourceMax: gl.getUniformLocation(easu, 'uSourceMax'),
  };
  const uRcas = {
    sharpness: gl.getUniformLocation(rcas, 'uSharpness'),
    origin: gl.getUniformLocation(rcas, 'uOrigin'),
    sourceMax: gl.getUniformLocation(rcas, 'uSourceMax'),
  };

  for (const tex of [sourceTex, midTex]) {
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }

  const sharpness = Math.pow(2, -(options.sharpnessStops ?? 0.25));
  const maxDpr = options.maxDevicePixelRatio ?? 2;
  let midW = 0;
  let midH = 0;
  let detached = false;
  let rvfcHandle = 0;
  let rafHandle = 0;
  let failed = false;

  const fail = (): void => {
    if (failed) return;
    failed = true;
    options.onFailure?.();
  };

  canvas.addEventListener('webglcontextlost', fail, { once: true });

  const draw = (): void => {
    if (detached || failed) return;
    const vw = video.videoWidth;
    const vh = video.videoHeight;
    if (!vw || !vh) return;

    const dpr = Math.min(window.devicePixelRatio || 1, maxDpr);
    const cw = Math.max(1, Math.round(canvas.clientWidth * dpr));
    const chh = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (canvas.width !== cw || canvas.height !== chh) {
      canvas.width = cw;
      canvas.height = chh;
    }

    // object-contain rect of the video inside the canvas.
    const scale = Math.min(cw / vw, chh / vh);
    const rw = Math.max(1, Math.round(vw * scale));
    const rh = Math.max(1, Math.round(vh * scale));
    const rx = Math.floor((cw - rw) / 2);
    const ry = Math.floor((chh - rh) / 2);

    // Upload the frame. Flipped once here so every later pass agrees with GL's
    // bottom-up rows; EASU's kernel is symmetric, so orientation does not change it.
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, sourceTex);
    try {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, video);
    } catch {
      return; // frame not ready; try again on the next callback
    }

    if (midW !== rw || midH !== rh) {
      midW = rw;
      midH = rh;
      gl.bindTexture(gl.TEXTURE_2D, midTex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, rw, rh, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    }

    gl.bindVertexArray(vao);

    // Pass 1: EASU into the intermediate texture at display size.
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, midTex, 0);
    gl.viewport(0, 0, rw, rh);
    gl.useProgram(easu);
    gl.bindTexture(gl.TEXTURE_2D, sourceTex);
    gl.uniform2f(uEasu.scale, vw / rw, vh / rh);
    gl.uniform2f(uEasu.bias, 0.5 * (vw / rw) - 0.5, 0.5 * (vh / rh) - 0.5);
    gl.uniform2i(uEasu.sourceMax, vw - 1, vh - 1);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // Pass 2: RCAS onto the canvas, letterbox cleared to black.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, cw, chh);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.viewport(rx, ry, rw, rh);
    gl.useProgram(rcas);
    gl.bindTexture(gl.TEXTURE_2D, midTex);
    gl.uniform1f(uRcas.sharpness, sharpness);
    gl.uniform2i(uRcas.origin, rx, ry);
    gl.uniform2i(uRcas.sourceMax, rw - 1, rh - 1);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  };

  const scheduleRvfc = (): void => {
    rvfcHandle = video.requestVideoFrameCallback(() => {
      draw();
      if (!detached && !failed) scheduleRvfc();
    });
  };
  const scheduleRaf = (): void => {
    rafHandle = window.requestAnimationFrame(() => {
      draw();
      if (!detached && !failed) scheduleRaf();
    });
  };
  if (typeof video.requestVideoFrameCallback === 'function') {
    scheduleRvfc();
  } else {
    scheduleRaf();
  }

  return {
    detach() {
      detached = true;
      if (rvfcHandle && typeof video.cancelVideoFrameCallback === 'function') {
        video.cancelVideoFrameCallback(rvfcHandle);
      }
      if (rafHandle) window.cancelAnimationFrame(rafHandle);
      gl.deleteTexture(sourceTex);
      gl.deleteTexture(midTex);
      gl.deleteFramebuffer(fbo);
      gl.deleteVertexArray(vao);
      gl.deleteProgram(easu);
      gl.deleteProgram(rcas);
    },
  };
}
