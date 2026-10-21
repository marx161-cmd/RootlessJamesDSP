// Ball + audio helix — 4-pass ShaderEditor shader.
// state: quaternion trackball physics (persistent 2-texel buffer).
// audio: reads sampler2D bands (108 raw float32 from JDSP), dB-normalizes and
//        settles per band into a 109-texel buffer (texel 108 = loudness).
// spark: glow accumulation (feedback) driven by the settled audio.
// image: renders the rotating ball; its surface is the parallel dual-helix
//        fibonacci lattice, each cell lit by its band's settled energy.

// Precision must come FIRST (before any shared function) — the preamble is
// prepended to every pass, so this one declaration covers all of them.
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

// ---- shared math ----
#define PI               3.141592653589793
#define GOLDEN_ANGLE     2.399963229728653
#define N_BANDS          108.0
#define PIN_SHARPNESS    110.0
#define SPREAD_FALLOFF   45.0
#define SPHERE_RADIUS    0.88

vec4 quatMul(vec4 q1, vec4 q2) {
    return vec4(
        q1.w * q2.xyz + q2.w * q1.xyz + cross(q1.xyz, q2.xyz),
        q1.w * q2.w - dot(q1.xyz, q2.xyz)
    );
}

vec4 quatSlerp(vec4 a, vec4 b, float t) {
    if (dot(a, b) < 0.0) b = -b;
    return normalize(mix(a, b, t));
}

mat3 quatToMat3Transposed(vec4 q) {
    vec4 sq = q * q;
    float xy = 2.0 * q.x * q.y, xz = 2.0 * q.x * q.z, yz = 2.0 * q.y * q.z;
    float wx = 2.0 * q.w * q.x, wy = 2.0 * q.w * q.y, wz = 2.0 * q.w * q.z;
    return mat3(
        1.0 - 2.0 * (sq.y + sq.z), xy - wz,                  xz + wy,
        xy + wz,                  1.0 - 2.0 * (sq.x + sq.z), yz - wx,
        xz - wy,                  yz + wx,                  1.0 - 2.0 * (sq.x + sq.y)
    );
}

vec2 intersectSphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float h = b * b - c;
    if (h < 0.0) return vec2(-1.0);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

// Parallel dual-helix fibonacci lattice on the unit sphere. Maps a local
// normal to the nearest of the 108 band pins. Bass (band 0) at +Z, treble
// (band 107) at -Z. O(1): bounded search window around the vertical estimate.
struct HelixCell {
    float bandIndex;
    float coreWeight;
    float tensionWeight;
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
    cell.coreWeight = exp(-d2 * PIN_SHARPNESS);
    cell.tensionWeight = exp(-d2 * SPREAD_FALLOFF);
    return cell;
}

// ---- Pass 1: State (ball physics) ----
==state==
uniform sampler2D state;
uniform vec2      resolution;
uniform vec2      touch;
uniform int       pointerCount;
uniform bool      screenLocked;
uniform vec3      linear;

#define TRACKBALL_GAIN    3.14159265
#define FRICTION          0.985
#define VEL_SMOOTH        0.65
#define ACCEL_THRESHOLD   3.2
#define ACCEL_GAIN        0.0035
#define RESTORE_SPEED     0.00015

void main() {
    if (gl_FragCoord.y > 1.0 || gl_FragCoord.x > 2.0) discard;

    vec4 q       = texture2D(state, vec2(0.5 / resolution.x, 0.5 / resolution.y));
    vec4 dynamic = texture2D(state, vec2(1.5 / resolution.x, 0.5 / resolution.y));

    vec2 vel       = dynamic.xy;
    vec2 lastTouch = dynamic.zw;

    if (dot(q, q) < 0.5) {
        q = vec4(0.0, 0.0, 0.0, 1.0);
    }

    bool isTouching = (pointerCount > 0) && !screenLocked;

    if (isTouching) {
        if (length(lastTouch) > 0.001) {
            vec2 delta = (touch - lastTouch) / resolution.y;
            vec2 instantVel = vec2(delta.y, -delta.x) * TRACKBALL_GAIN;
            vel = mix(vel, instantVel, VEL_SMOOTH);
        }
        lastTouch = touch;
    } else {
        vec2 rawAccel = vec2(linear.y, -linear.x);
        float accelMag = length(rawAccel);
        if (accelMag > ACCEL_THRESHOLD) {
            float excess = accelMag - ACCEL_THRESHOLD;
            float impulse = pow(excess, 2.2) * ACCEL_GAIN;
            vel += (rawAccel / accelMag) * impulse;
        }
        vel *= FRICTION;
        lastTouch = vec2(0.0);
        if (length(vel) < 0.015) {
            q = quatSlerp(q, vec4(0.0, 0.0, 0.0, 1.0), RESTORE_SPEED);
        }
    }

    float angle = length(vel);
    vec4 deltaQ = vec4(0.0, 0.0, 0.0, 1.0);
    if (angle > 1e-5) {
        vec3 axis = normalize(vec3(vel.x, vel.y, 0.0));
        float halfAngle = angle * 0.5;
        deltaQ = vec4(axis * sin(halfAngle), cos(halfAngle));
    }

    q = normalize(quatMul(deltaQ, q));

    if (gl_FragCoord.x < 1.0) {
        gl_FragColor = q;
    } else {
        gl_FragColor = vec4(vel, lastTouch);
    }
}

// ---- Pass 2: Audio (dB normalize + settle) ----
==audio==
uniform sampler2D bands;   // 108x1 raw float32 from JDSP (linear RMS)
uniform sampler2D audio;   // self-feedback (previous frame)
uniform vec2      resolution;

// Tune these against the phone's actual raw range (log2 of linear RMS).
#define AUDIO_FLOOR    -16.0
#define AUDIO_CEIL     -2.0
#define SMOOTH_ATTACK  0.6
#define SMOOTH_RELEASE 0.10

void main() {
    if (gl_FragCoord.y >= 1.0 || gl_FragCoord.x >= 109.0) discard;

    // Texel 108 = loudness = max normalized band (same 0..1 scale).
    if (gl_FragCoord.x >= 108.0) {
        float m = 0.0;
        for (int i = 0; i < 108; i++) {
            m = max(m, texture2D(bands, vec2((float(i) + 0.5) / N_BANDS, 0.5)).r);
        }
        float db = log2(max(m, 1e-9));
        float level = clamp((db - AUDIO_FLOOR) / (AUDIO_CEIL - AUDIO_FLOOR), 0.0, 1.0);
        gl_FragColor = vec4(level, 0.0, 0.0, 0.0);
        return;
    }

    float band = gl_FragCoord.x;
    float raw = texture2D(bands, vec2((band + 0.5) / N_BANDS, 0.5)).r;
    float db = log2(max(raw, 1e-9));
    float target = clamp((db - AUDIO_FLOOR) / (AUDIO_CEIL - AUDIO_FLOOR), 0.0, 1.0);

    float prev = texture2D(audio, vec2((band + 0.5) / resolution.x, 0.5 / resolution.y)).r;
    float rate = (target > prev) ? SMOOTH_ATTACK : SMOOTH_RELEASE;
    gl_FragColor = vec4(mix(prev, target, rate), 0.0, 0.0, 0.0);
}

// ---- Pass 3: Spark (glow accumulation) ----
==spark==
uniform sampler2D spark;
uniform sampler2D state;
uniform sampler2D audio;
uniform vec2      resolution;
uniform float     time;

#define GLOW_DECAY       0.84
#define GLOW_DECAY_LOUD  0.91
#define FLOW_BASE        0.004
#define FLOW_AUDIO       0.015

void main() {
    vec2 st = gl_FragCoord.xy / resolution.xy;
    vec2 uv = (-resolution.xy + 2.0 * gl_FragCoord.xy) / resolution.y;

    // Loudness from the audio buffer's level texel.
    float level = texture2D(audio, vec2(108.5 / resolution.x, 0.5 / resolution.y)).r;

    // Spiral flow toward the sphere center, faster when loud.
    vec2 uvToSt = vec2(resolution.y / (2.0 * resolution.x), 0.5);
    float r = length(uv) + 1e-4;
    vec2 flow = (uv / r) * (FLOW_BASE + level * FLOW_AUDIO) * smoothstep(0.0, 0.3, r);
    vec2 src = st - flow * uvToSt;

    vec3 history = texture2D(spark, src).rgb;
    vec2 edge = step(vec2(0.0), src) * step(src, vec2(1.0));
    history *= edge.x * edge.y;

    // Rotated local normal for this pixel (if on the sphere).
    vec4 quat = texture2D(state, vec2(0.5 / resolution.x, 0.5 / resolution.y));
    vec3 ro = vec3(0.0, 0.0, -2.4);
    vec3 rd = normalize(vec3(uv, 1.8));
    vec2 hit = intersectSphere(ro, rd, SPHERE_RADIUS);

    float emit = 0.0;
    if (hit.x >= 0.0) {
        vec3 worldNormal = normalize(ro + rd * hit.x);
        vec3 localNormal = quatToMat3Transposed(quat) * worldNormal;
        HelixCell cell = evaluateHelixLattice(localNormal);
        float energy = texture2D(audio, vec2((cell.bandIndex + 0.5) / resolution.x, 0.5 / resolution.y)).r;
        emit = energy * (cell.coreWeight + 0.35 * cell.tensionWeight);
    }

    float decay = mix(GLOW_DECAY, GLOW_DECAY_LOUD, clamp(level * 1.5, 0.0, 1.0));
    gl_FragColor = vec4(max(vec3(emit), history * decay), emit);
}

// ---- Pass 4: Image (render) ----
==image==
uniform sampler2D spark;
uniform sampler2D state;
uniform sampler2D audio;
uniform vec2      resolution;
uniform float     time;

#define GLOW_GAIN   0.55

void main() {
    vec2 st = gl_FragCoord.xy / resolution.xy;
    vec2 uv = (-resolution.xy + 2.0 * gl_FragCoord.xy) / resolution.y;

    vec4 quat = texture2D(state, vec2(0.5 / resolution.x, 0.5 / resolution.y));
    float level = texture2D(audio, vec2(108.5 / resolution.x, 0.5 / resolution.y)).r;

    vec3 ro = vec3(0.0, 0.0, -2.4);
    vec3 rd = normalize(vec3(uv, 1.8));

    // Slight audio-driven contraction.
    float kick = pow(clamp(level, 0.0, 1.0), 1.6) * 0.12;
    float dynamicRadius = SPHERE_RADIUS * (1.0 - kick);

    vec2 hit = intersectSphere(ro, rd, dynamicRadius);
    if (hit.x < 0.0) {
        // Outside the ball: still show the glow field faintly.
        vec3 glow = texture2D(spark, st).rgb;
        gl_FragColor = vec4(glow * GLOW_GAIN * 0.5, 1.0);
        return;
    }

    vec3 hitPos      = ro + rd * hit.x;
    vec3 worldNormal = normalize(hitPos);
    vec3 localNormal = quatToMat3Transposed(quat) * worldNormal;

    HelixCell cell = evaluateHelixLattice(localNormal);
    float energy = texture2D(audio, vec2((cell.bandIndex + 0.5) / resolution.x, 0.5 / resolution.y)).r;

    // Cell brightness: sharp pin core + soft tension membrane, gated by energy.
    float cellBright = energy * (cell.coreWeight + 0.4 * cell.tensionWeight);

    // Heat color by pitch (bass red -> treble cyan).
    float pitch = cell.bandIndex / (N_BANDS - 1.0);
    vec3 low  = vec3(1.0, 0.25, 0.1);
    vec3 mid  = vec3(1.0, 0.9, 0.3);
    vec3 high = vec3(0.1, 0.7, 1.0);
    vec3 tint = mix(low, mid, smoothstep(0.0, 0.5, pitch));
    tint = mix(tint, high, smoothstep(0.5, 1.0, pitch));

    // Idle surface + lit cells.
    vec3 idle = vec3(0.03, 0.04, 0.06);
    vec3 col = mix(idle, tint, clamp(cellBright, 0.0, 1.0));

    // Limb darkening.
    float NdotV = clamp(dot(-rd, worldNormal), 0.0, 1.0);
    col *= pow(NdotV, 0.65);

    // Glow accumulation.
    vec2 off = 2.12132 / resolution.xy;
    vec3 glow = (texture2D(spark, st + vec2( off.x,  off.y)).rgb +
                 texture2D(spark, st + vec2(-off.x,  off.y)).rgb +
                 texture2D(spark, st + vec2( off.x, -off.y)).rgb +
                 texture2D(spark, st + vec2(-off.x, -off.y)).rgb) * 0.25;
    vec3 sparkSample = texture2D(spark, st).rgb;

    vec3 outColor = col + (sparkSample * 0.4 + glow * 0.6) * GLOW_GAIN;
    outColor *= 1.0 - smoothstep(1.0, 1.6, length(uv));

    gl_FragColor = vec4(outColor, 1.0);
}
