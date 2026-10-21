// Hexagon circle (port of desktop tonnetz_pins polar layout).
// audio: reads sampler2D bands (108 raw float32), dB-normalizes + settles.
// image: top-down polar wheel — angle = pitch class (12 spokes), radius =
//        octave (9 rings); each hexagonal cell lights with its band energy.

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

#define PI          3.141592653589793
#define N_BANDS     108.0
#define ROWS        9.0
#define COLS        12.0

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

// ---- Pass 1: Audio (dB normalize + settle) ----
==audio==
uniform sampler2D bands;
uniform sampler2D audio;
uniform vec2      resolution;

#define AUDIO_FLOOR    -16.0
#define AUDIO_CEIL     -2.0
#define SMOOTH_ATTACK  0.6
#define SMOOTH_RELEASE 0.10

void main() {
    if (gl_FragCoord.y >= 1.0 || gl_FragCoord.x >= N_BANDS) discard;
    float band = gl_FragCoord.x;
    float raw = texture2D(bands, vec2((band + 0.5) / N_BANDS, 0.5)).r;
    float db = log2(max(raw, 1e-9));
    float target = clamp((db - AUDIO_FLOOR) / (AUDIO_CEIL - AUDIO_FLOOR), 0.0, 1.0);
    float prev = texture2D(audio, vec2((band + 0.5) / resolution.x, 0.5 / resolution.y)).r;
    float rate = (target > prev) ? SMOOTH_ATTACK : SMOOTH_RELEASE;
    gl_FragColor = vec4(mix(prev, target, rate), 0.0, 0.0, 0.0);
}

// ---- Pass 2: Image (polar hex wheel) ----
==image==
uniform sampler2D audio;
uniform vec2      resolution;
uniform float     time;

#define HEX_SIZE      0.5
#define HUB_RADIUS    1.4
#define RING_PITCH    2.0

// Angled perspective camera, look-at the wheel center from above + front.
#define CAM_HEIGHT    16.0
#define CAM_DIST      13.0
#define CAM_FOV       1.6

void main() {
    vec3 cam = vec3(0.0, CAM_HEIGHT, CAM_DIST);
    vec3 fwd = normalize(-cam);
    vec3 right = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(right, fwd);

    vec2 uv = (gl_FragCoord.xy / resolution.xy - 0.5) * 2.0;
    uv.x *= resolution.x / resolution.y;

    vec3 rd = normalize(fwd + uv.x * right * CAM_FOV + uv.y * up * CAM_FOV);
    if (rd.y >= -0.001) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    float t = -cam.y / rd.y;
    vec3 hit = cam + rd * t;
    vec2 xz = hit.xz;

    vec2 id, local;
    hexInfo(xz, HEX_SIZE, id, local);
    vec2 tileCenter = xz - local;

    float r = length(tileCenter);
    float theta = atan(tileCenter.y, tileCenter.x);
    if (theta < 0.0) theta += 2.0 * PI;

    float energy = 0.0;
    float row = -1.0;
    float acc = HUB_RADIUS;
    for (int i = 0; i < 9; i++) {
        if (r < acc + RING_PITCH) { row = float(i); break; }
        acc += RING_PITCH;
    }

    if (r < HUB_RADIUS) {
        // Center hub: max bass (row 0) across all 12 columns.
        float bass = 0.0;
        for (int c = 0; c < 12; c++) {
            bass = max(bass, texture2D(audio, vec2((float(c) + 0.5) / resolution.x, 0.5 / resolution.y)).r);
        }
        energy = bass;
    } else if (row >= 0.0) {
        int col = int(mod(floor(theta / (2.0 * PI / COLS)), COLS));
        int band = int(row) * 12 + col;
        energy = texture2D(audio, vec2((float(band) + 0.5) / resolution.x, 0.5 / resolution.y)).r;
    }

    // Hexagon fill + rim.
    float inr = 0.4330127; // sqrt(3)/2 * HEX_SIZE
    float d = hexEdgeDist(local, inr);
    float aa = smoothstep(0.02, 0.0, d);
    float rim = smoothstep(-0.05, -0.01, d);

    // Heat ramp: dim teal -> bright magenta with energy.
    vec3 c = mix(vec3(0.01, 0.03, 0.05), vec3(0.0, 0.55, 0.6), smoothstep(0.0, 0.5, energy));
    c = mix(c, vec3(0.9, 0.1, 0.35), smoothstep(0.5, 0.95, energy));

    // Fade the whole wheel out past its outer edge.
    float fade = 1.0 - smoothstep(WHEEL * 0.85, WHEEL, r);
    c = mix(vec3(0.0), c, aa) * fade;
    c = mix(c, vec3(0.02, 0.03, 0.05), rim * aa);

    gl_FragColor = vec4(c, 1.0);
}
