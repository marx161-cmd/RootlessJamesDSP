// Tonnetz polar hex wheel — faithful port of desktop tonnetz_pins.mp.
// field: reads 108 raw float32 bands, dB-normalizes, then the full buffer_a
//        cleanup (harmonic subtraction -> flatness gate -> treble boost +
//        gamma -> settle -> pull/bridge -> drag), one texel per cell.
// image: the desktop's hex-column raymarch + polar sampleField, reading field.

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

#define PI            3.141592653589793
#define N_BANDS       108.0
#define ROWS          9.0
#define COLS          12.0

// ---- pointy-top axial hex grid (floor-based, ES 1.00 safe) ----
vec2 hexRound(vec2 axial) {
    float x = axial.x, z = axial.y, y = -x - z;
    float rx = floor(x + 0.5), ry = floor(y + 0.5), rz = floor(z + 0.5);
    float xD = abs(rx - x), yD = abs(ry - y), zD = abs(rz - z);
    if (xD > yD && xD > zD) { rx = -ry - rz; }
    else if (yD > zD) { ry = -rx - rz; }
    else { rz = -rx - ry; }
    return vec2(rx, rz);
}
vec2 hexCenter(vec2 c, float s) {
    return s * vec2(1.7320508 * c.x + 0.8660254 * c.y, 1.5 * c.y);
}
void hexInfo(vec2 p, float s, out vec2 id, out vec2 local) {
    float q = (0.57735027 * p.x - 0.33333333 * p.y) / s;
    float r = (0.66666667 * p.y) / s;
    id = hexRound(vec2(q, r));
    local = p - hexCenter(id, s);
}
// Pointy-top hexagon signed distance (0 at boundary, -inradius at center).
float hexEdgeDist(vec2 p, float inradius) {
    float c30 = 0.8660254, s30 = 0.5;
    p = vec2(c30 * p.x - s30 * p.y, s30 * p.x + c30 * p.y);
    vec3 k = vec3(-0.8660254, 0.5, 0.57735027);
    vec2 q = abs(p);
    q -= 2.0 * min(dot(k.xy, q), 0.0) * k.xy;
    vec2 d = vec2(clamp(q.x, -k.z * inradius, k.z * inradius), inradius);
    return length(q - d) * sign(q.y - inradius);
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// ---- polar layout ----
#define HUB_RADIUS  1.4
#define RING_PITCH  2.0
#define RING_GROWTH 0.06
float ringPitchAt(float row) { return RING_PITCH * (1.0 + RING_GROWTH * row); }

// ---- height / colour scale ----
#define HEIGHT_GAIN  8.0
#define HEIGHT_CEIL  5.0
#define HEIGHT_SCALE 1.5
#define BASE_HEIGHT  0.05
#define STUD_HEIGHT  0.045
#define BEVEL_FLAT   0.85
#define PLANE_HEIGHT 1.8
#define PLANE_SOFT   0.8

#define DT (1.0 / 60.0)

// ==================== Pass 1: field (cleanup) ====================
==field==
uniform sampler2D bands;  // 108 raw float32 (linear RMS)
uniform sampler2D field;  // self-feedback (previous frame)
uniform vec2      resolution;

// Desktop norm_db: (20*log10(mag) + 60) / 60, i.e. 0 dBFS -> 1.0, -60 dB -> 0.0.
// The cleanup coefficients below (0.30/0.15/0.35/2.0/gamma) are tuned against
// THIS scale, so the per-band cleanup math must run on exactly these 0..1 values.
float rawNorm(int b) {
    float raw = texture2D(bands, vec2((float(b) + 0.5) / N_BANDS, 0.5)).r;
    float db = 6.0206 * log2(max(raw, 1e-9)); // 20*log10 = 6.0206*log2
    return clamp((db + 60.0) / 60.0, 0.0, 1.0);
}
float prevG(int b) {
    return texture2D(field, vec2((float(b) + 0.5) / resolution.x, 0.5 / resolution.y)).g;
}
float prevB(int b) {
    return texture2D(field, vec2((float(b) + 0.5) / resolution.x, 0.5 / resolution.y)).b;
}

void main() {
    if (gl_FragCoord.y >= 1.0 || gl_FragCoord.x >= N_BANDS) discard;
    int band = int(gl_FragCoord.x);
    int row = band / 12;
    int col = band - row * 12;

    float peak = rawNorm(band);

    // Stage 1: vertical harmonic subtraction.
    if (row > 0) {
        peak = max(0.0, peak - 0.30 * rawNorm(band - 12));
        int fifthCol = int(mod(float(col - 7), 12.0));
        peak = max(0.0, peak - 0.15 * rawNorm((row - 1) * 12 + fifthCol));
    }

    // Stage 2: spectral flatness gate.
    float octaveMean = 0.0;
    for (int c = 0; c < 12; c++) octaveMean += rawNorm(row * 12 + c);
    octaveMean /= 12.0;
    float floorFactor = 0.35 + 0.025 * float(row);
    peak = max(0.0, peak - floorFactor * octaveMean);

    // Treble boost + gamma.
    float regBoost = 1.0 + (2.0 - 1.0) * (float(row) / (ROWS - 1.0));
    float gamma = 1.2 - 0.05 * float(row);
    float e = pow(max(peak * regBoost, 0.0), gamma);
    float target = clamp(e * HEIGHT_GAIN, 0.0, HEIGHT_CEIL);

    // Settle (asymmetric attack/release).
    float prevH = prevG(band);
    float tauUp = 0.0025 + 0.0012 * float(row);
    float tau = (target > prevH) ? tauUp : tauUp * 30.0;
    float alpha = clamp(1.0 - exp(-DT / tau), 0.0, 1.0);
    float cleanH = mix(prevH, target, alpha);

    // Pull (column max) + bridge (min of neighbours).
    float colMain = 0.0;
    for (int r = 0; r < 9; r++) colMain = max(colMain, prevG(col + r * 12));
    int leftCol = int(mod(float(col - 1), 12.0));
    int rightCol = int(mod(float(col + 1), 12.0));
    float leftMain = 0.0, rightMain = 0.0;
    for (int r = 0; r < 9; r++) {
        leftMain = max(leftMain, prevG(leftCol + r * 12));
        rightMain = max(rightMain, prevG(rightCol + r * 12));
    }
    float dispH = max(cleanH, 0.03 * colMain);
    dispH = max(dispH, 0.45 * min(leftMain, rightMain));

    // Drag (slow copy of clean).
    float dragAlpha = clamp(1.0 - exp(-DT / 1.4), 0.0, 1.0);
    float dragH = mix(prevB(band), cleanH, dragAlpha);

    gl_FragColor = vec4(dispH, cleanH, dragH, 0.0);
}

// ==================== Pass 2: image (render) ====================
// ==================== Pass 2: image (cheap flat fill, no raymarch) ====================
==image==
uniform sampler2D field;
uniform vec2      resolution;
uniform float     time;

#define S 0.5

float fieldAt(int col, int row) {
    int band = row * 12 + col;
    return texture2D(field, vec2((float(band) + 0.5) / resolution.x, 0.5 / resolution.y)).r;
}

void main() {
    vec2 aspect = vec2(resolution.x / resolution.y, 1.0);
    vec2 uv = (gl_FragCoord.xy / resolution.xy - 0.5) * 2.0;
    vec2 xz = uv * aspect * 20.0;

    vec2 id, local;
    hexInfo(xz, S, id, local);
    vec2 tileCenter = xz - local;

    float r = length(tileCenter);
    float theta = atan(tileCenter.y, tileCenter.x);
    if (theta < 0.0) theta += 2.0 * PI;

    float height = 0.0;
    if (r < HUB_RADIUS) {
        float treble = 0.0;
        for (int c = 0; c < 12; c++) treble = max(treble, fieldAt(c, 8));
        height = treble;
    } else {
        float candRow = -1.0;
        float acc = HUB_RADIUS;
        for (int i = 0; i < 9; i++) {
            float pitch = ringPitchAt(float(i));
            if (r < acc + pitch) { candRow = float(i); break; }
            acc += pitch;
        }
        if (candRow >= 0.0) {
            int col = int(mod(floor(theta / (2.0 * PI / COLS)), COLS));
            int dataRow = 8 - int(candRow); // flip: treble center, bass rim
            height = fieldAt(col, dataRow);
        }
    }

    // Hexagon fill + rim.
    float inr = S * 0.8660254;
    float d = hexEdgeDist(local, inr);
    float aa = smoothstep(0.02, 0.0, d);

    // Same magenta-above-a-plane colour as the desktop fieldColor.
    float topElev = height * HEIGHT_SCALE;
    float above = smoothstep(PLANE_HEIGHT, PLANE_HEIGHT + PLANE_SOFT, topElev);
    vec3 col = mix(vec3(0.05, 0.06, 0.08), vec3(0.92, 0.05, 0.32), above);

    // Seam glow between tiles.
    float dTop = (d + inr) / inr;
    float underGlow = smoothstep(0.82, 1.0, dTop) * (1.0 - above);
    col += vec3(0.95, 0.05, 0.13) * underGlow;

    // Fade the wheel edge.
    float fade = 1.0 - smoothstep(16.0, 19.5, r);
    col = mix(vec3(0.0), col, aa) * fade;

    gl_FragColor = vec4(col, 1.0);
}
