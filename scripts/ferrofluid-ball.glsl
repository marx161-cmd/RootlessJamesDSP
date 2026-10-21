// Ferrofluid ball (port of desktop ferrofluid.mp) — simplified.
// audio: reads sampler2D bands (108 raw float32), dB-normalizes + settles.
// image: raymarched sphere whose radius is pushed outward by per-cell spikes
//        (fibonacci dual-helix lattice), shaded dark-chrome + iridescence.

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

#define PI          3.141592653589793
#define GOLDEN_ANGLE 2.399963229728653
#define N_BANDS     108.0

// ---- fibonacci dual-helix lattice ----
struct HelixCell {
    float bandIndex;
    float coreWeight;
};

HelixCell evaluateHelixLattice(vec3 n) {
    HelixCell cell;
    float t = clamp(1.0 - (n.z * 0.5 + 0.5), 0.0, 1.0);
    float approxIdx = clamp(floor(t * N_BANDS), 0.0, N_BANDS - 1.0);
    float minDist = 1e5;
    float bestIdx = approxIdx;
    for (int k = -1; k <= 1; k++) {
        float idx = clamp(approxIdx + float(k), 0.0, N_BANDS - 1.0);
        float z = 1.0 - (2.0 * idx + 1.0) / N_BANDS;
        float r = sqrt(max(0.0, 1.0 - z * z));
        float basePhi = idx * GOLDEN_ANGLE;
        vec3 pinA = vec3(r * cos(basePhi), r * sin(basePhi), z);
        float dA = distance(n, pinA);
        if (dA < minDist) { minDist = dA; bestIdx = idx; }
        vec3 pinB = vec3(r * cos(basePhi + PI), r * sin(basePhi + PI), z);
        float dB = distance(n, pinB);
        if (dB < minDist) { minDist = dB; bestIdx = idx; }
    }
    cell.bandIndex = bestIdx;
    float d2 = minDist * minDist;
    cell.coreWeight = exp(-d2 * 25.0); // SPIKE_SHARPNESS
    return cell;
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

// ---- Pass 2: Image (raymarched spike ball) ----
==image==
uniform sampler2D audio;
uniform vec2      resolution;
uniform float     time;

#define BASE_RADIUS   0.54
#define SPIKE_SCALE   0.10
#define SPIN_SPEED    0.15
#define SPIN_SPEED_2  0.09

vec3 rotateY(vec3 v, float a) {
    float s = sin(a), c = cos(a);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

vec3 rotateX(vec3 v, float a) {
    float s = sin(a), c = cos(a);
    return vec3(v.x, c * v.y - s * v.z, s * v.y + c * v.z);
}

float mapSDF(vec3 p) {
    vec3 rel = p - vec3(0.0, sin(time * 0.5) * 0.12, 0.0);
    float r = length(rel);
    vec3 n = rel / max(r, 0.0001);
    n = rotateY(n, -time * SPIN_SPEED);
    n = rotateX(n, -time * SPIN_SPEED_2);
    HelixCell cell = evaluateHelixLattice(n);
    float energy = texture2D(audio, vec2((cell.bandIndex + 0.5) / resolution.x, 0.5 / resolution.y)).r;
    return r - (BASE_RADIUS + energy * cell.coreWeight * SPIKE_SCALE);
}

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * resolution.xy) / resolution.y;
    vec3 ro = vec3(0.0, 0.0, 5.2);
    vec3 rd = normalize(vec3(uv, -1.6));

    // Analytic bounding-sphere entry.
    float boundR = BASE_RADIUS + SPIKE_SCALE + 0.1;
    vec3 oc = ro - vec3(0.0, sin(time * 0.5) * 0.12, 0.0);
    float b = dot(oc, rd);
    float c = dot(oc, oc) - boundR * boundR;
    float disc = b * b - c;
    if (disc < 0.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    float t = max(-b - sqrt(disc), 0.0);
    float tMax = -b + sqrt(disc);

    bool hit = false;
    for (int i = 0; i < 48; i++) {
        vec3 p = ro + rd * t;
        float d = mapSDF(p);
        if (d < 0.008) { hit = true; break; }
        t += d * 0.7;
        if (t > tMax) break;
    }
    if (!hit) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    vec3 p = ro + rd * t;
    vec2 e = vec2(0.015, -0.015);
    vec3 normal = normalize(
        e.xyy * mapSDF(p + e.xyy) + e.yyx * mapSDF(p + e.yyx) +
        e.yxy * mapSDF(p + e.yxy) + e.xxx * mapSDF(p + e.xxx)
    );

    vec3 lightDir = normalize(vec3(0.5, 0.8, 0.6));
    vec3 viewDir = normalize(ro - p);
    vec3 halfV = normalize(lightDir + viewDir);

    float diff = max(dot(normal, lightDir), 0.0);
    float spec = pow(max(dot(normal, halfV), 0.0), 60.0);
    float fres = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0);

    // Dark chrome.
    vec3 col = vec3(0.02, 0.02, 0.026) * (0.2 + 0.85 * diff);
    col += vec3(0.4, 1.0, 0.85) * spec * 0.58;

    // Iridescent rim.
    vec3 irid = 0.5 + 0.5 * cos(vec3(0.0, 2.0, 4.0) + fres * 6.2831);
    col += irid * fres * 0.045;

    col = col / (1.0 + col);
    gl_FragColor = vec4(col, 1.0);
}
