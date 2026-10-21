#version 300 es
// ==================== Shared preamble (prepended to every pass) ====================
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
precision highp sampler2D;
#else
precision mediump float;
precision mediump sampler2D;
#endif

out vec4 fragColor;

#define N_BANDS          108.0
#define PI               3.141592653589793
#define TAU              6.283185307179586
#define FPS              120.0
#define SPHERE_RADIUS    0.88
#define SENSOR_POS       vec2(0.0, 0.0)
#define BALL_ZOOM        1.0

#define N_GEARS          8
#define OMEGA_MAX        0.6

vec2 pack16(float v) {
    float x  = floor(clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
    float hi = floor(x / 256.0);
    return vec2(hi, x - hi * 256.0) / 255.0;
}
float unpack16(vec2 c) {
    vec2 b = floor(c * 255.0 + 0.5);
    return (b.x * 256.0 + b.y) / 65535.0;
}
vec2 encS(float v, float range) {
    float x  = floor(clamp(v / range, -1.0, 1.0) * 32767.0 + 32767.5);
    float hi = floor(x / 256.0);
    return vec2(hi, x - hi * 256.0) / 255.0;
}
float decS(vec2 c, float range) {
    vec2 b = floor(c * 255.0 + 0.5);
    return (b.x * 256.0 + b.y - 32767.0) / 32767.0 * range;
}

vec4 quatMul(vec4 a, vec4 b) {
    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz),
                a.w * b.w - dot(a.xyz, b.xyz));
}

mat3 quatToMat3(vec4 q) {
    float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    float xx = q.x * x2,  xy = q.x * y2,  xz = q.x * z2;
    float yy = q.y * y2,  yz = q.y * z2,  zz = q.z * z2;
    float wx = q.w * x2,  wy = q.w * y2,  wz = q.w * z2;
    return mat3(
        1.0 - (yy + zz), xy + wz,         xz - wy,
        xy - wz,         1.0 - (xx + zz), yz + wx,
        xz + wy,         yz - wx,         1.0 - (xx + yy)
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

// Orbital tilt per gear: mostly horizontal, a couple of slanted outliers.
float gearTilt(float idx) {
    if (idx == 3.0) return 0.38;
    if (idx == 6.0) return -0.5;
    return 0.0;
}

// Deterministic per-gear phase offset so the system doesn't start aligned.
float gearPhaseOffset(float idx) {
    return fract(idx * 0.6180339887);
}

// --- planet detailing ---
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

// Varied sizes: two gas giants (one ringed), one ice giant, small rocky rest.
float planetRadius(float idx) {
    if (idx == 2.0) return 0.044;   // gas giant
    if (idx == 5.0) return 0.050;   // ringed gas giant
    if (idx == 7.0) return 0.030;   // ice giant
    return 0.013 + idx * 0.0016;    // rocky
}

float isGasGiant(float idx) {
    return (idx == 2.0 || idx == 5.0) ? 1.0 : 0.0;
}

float hasRing(float idx) {
    return (idx == 5.0) ? 1.0 : 0.0;
}

// Banded gas-giant surface (horizontal stripes + a storm spot).
vec3 gasGiantColor(vec3 n, float idx, float time) {
    vec3 spin = (idx == 2.0) ? normalize(vec3(0.5, 0.82, 0.28))
                             : normalize(vec3(-0.42, 0.86, 0.30));
    float lat = dot(n, spin);
    float band = sin(lat * 12.0 + 0.6 * sin(time * 0.3));
    vec3 cream = vec3(0.85, 0.74, 0.55);
    vec3 brown = vec3(0.55, 0.40, 0.27);
    vec3 c = mix(cream, brown, band * 0.5 + 0.5);
    vec3 spotDir = normalize(vec3(0.35, 0.55, 0.76));
    float spot = exp(-(1.0 - dot(n, spotDir)) * 12.0);
    c = mix(c, vec3(0.85, 0.22, 0.16), spot * 0.7);
    return c;
}

// ==================== Pass 1: State & Mechanical Gearing ====================
==state 128x3==
uniform sampler2D state;
uniform sampler2D bands;
uniform vec2      resolution;
uniform vec2      surfaceResolution;
uniform vec2      touch;
uniform int       pointerCount;
uniform bool      screenLocked;
uniform vec3      linear;
uniform vec3      gravity;

#define TRACKBALL_GAIN      2.6
#define FLING_DECAY         0.92
#define FRICTION            0.965
#define GRAVITY_TORQUE_GAIN 0.00022
#define GRAV_TERMINAL_SPEED 0.08
#define RESTORE_SPEED_LOCK  0.35
#define FLICK_THRESH        4.5
#define VIRTUAL_KICK_SPEED  0.22
#define COOLDOWN_FRAMES     22.0
#define RESTORE_SPRING_GAIN  0.00035   // How firmly it pulls toward vertical alignment
#define RESTORE_VEL_THRESH   0.08  // Only engage when spin has slowed down below this speed

vec4 slot(float px) {
    return texture(state, vec2((px + 0.5) / resolution.x, 0.5 / resolution.y));
}
float rdS(float px, float range) { return decS(slot(px).rg, range); }

vec4 quatSlerp(vec4 a, vec4 b, float t) {
    if (dot(a, b) < 0.0) b = -b;
    return normalize(mix(a, b, t));
}
vec2 touchToUv(vec2 t) {
    return (-surfaceResolution.xy + 2.0 * t) / surfaceResolution.y;
}
vec3 trackball(vec2 uv) {
    vec2 c = uv / (SPHERE_RADIUS * BALL_ZOOM);
    float r2 = dot(c, c);
    if (r2 <= 1.0) return vec3(c, -sqrt(1.0 - r2));
    return vec3(normalize(c), 0.0);
}

void main() {
    // ------------------------------------------------------------ Row 0: Physics & Mechanics
    if (gl_FragCoord.y < 1.0) {
        float px = floor(gl_FragCoord.x);
        if (px > 25.0) discard;

        // ---- Physics integration (px 0..11) ----
        vec4 q      = vec4(rdS(0.0, 1.0), rdS(1.0, 1.0), rdS(2.0, 1.0), rdS(3.0, 1.0));
        vec3 omega  = vec3(rdS(4.0, OMEGA_MAX), rdS(5.0, OMEGA_MAX), rdS(6.0, OMEGA_MAX));
        vec3 fling  = vec3(rdS(7.0, OMEGA_MAX), rdS(8.0, OMEGA_MAX), rdS(9.0, OMEGA_MAX));
        vec2 lastTouch = vec2(unpack16(slot(10.0).rg), unpack16(slot(11.0).rg)) * surfaceResolution;
        bool wasTouching = (lastTouch.x + lastTouch.y) > 0.5;

        float qq = dot(q, q);
        if (!(qq > 0.5 && qq < 1.5)) {
            q = vec4(0.0, 0.0, 0.0, 1.0);
            omega = vec3(0.0);
            fling = vec3(0.0);
            wasTouching = false;
        }
        q = normalize(q);

        bool isTouching = (pointerCount > 0) && !screenLocked;

        if (isTouching) {
            if (!wasTouching) { omega = vec3(0.0); fling = vec3(0.0); }

            vec3 meas = vec3(0.0);
            if (wasTouching) {
                vec3 v0 = trackball(touchToUv(lastTouch));
                vec3 v1 = trackball(touchToUv(touch));
                vec3 ax = cross(v0, v1);
                float s = length(ax);
                if (s > 1e-6) meas = ax * (asin(min(s, 1.0)) / s) * TRACKBALL_GAIN;
            }

            vec3 avg = 0.5 * (meas + omega);
            fling *= FLING_DECAY;
            if (dot(avg, avg) > dot(fling, fling)) fling = avg;

            omega = meas;
            lastTouch = touch;
        } else {
            if (wasTouching) omega = fling;
            lastTouch = vec2(0.0);

            if (screenLocked) {
                omega *= 0.72;
                q = quatSlerp(q, vec4(0.0, 0.0, 0.0, 1.0), RESTORE_SPEED_LOCK);
            } else {
                omega *= FRICTION;

                // ---- Vertical Orbital Tramline Bias ----
                // When spin drops below threshold, apply gentle restorative torque toward identity (0,0,0,1)
                float spinRate = length(omega);
                if (spinRate < RESTORE_VEL_THRESH) {
                    // Quaternion error vector: imaginary part of (q^-1 * q_target)
                    // With q_target = (0,0,0,1), error torque is directly proportional to -q.xyz * sign(q.w)
                    vec3 alignTorque = -q.xyz * sign(q.w);

                    // Smoothly ramp in the spring torque as speed decreases
                    float springWeight = smoothstep(RESTORE_VEL_THRESH, 0.01, spinRate);
                    omega += alignTorque * (RESTORE_SPRING_GAIN * springWeight);
                }
            }
        }

        // Shove / Flick impulse
        float flickTimer = slot(16.0).r;
        float shoveSq = dot(linear.xy, linear.xy);
        if (flickTimer <= (1.0 / COOLDOWN_FRAMES) && shoveSq > (FLICK_THRESH * FLICK_THRESH)) {
            vec2 sdir = linear.xy * inversesqrt(shoveSq);
            omega += vec3(sdir.y, -sdir.x, 0.0) * VIRTUAL_KICK_SPEED;
        }

        // Gravity Torque
        vec3 contactNormal = vec3(0.0, 0.0, 1.0);
        vec3 gravTorque = cross(contactNormal, gravity);
        float curSpd = length(omega);
        if (curSpd < GRAV_TERMINAL_SPEED) {
            omega += gravTorque * GRAVITY_TORQUE_GAIN;
        }

        float spd = length(omega);
        if (spd > OMEGA_MAX) omega *= OMEGA_MAX / spd;

        float ang = length(omega);
        if (ang > 1e-6) {
            vec4 dq = vec4(omega / ang * sin(ang * 0.5), cos(ang * 0.5));
            q = normalize(quatMul(dq, q));
        }

        // px16: flick lockout cooldown counter
        if (px == 16.0) {
            float timer = slot(16.0).r;
            timer = max(0.0, timer - (1.0 / COOLDOWN_FRAMES));
            if (timer <= 0.0 && dot(linear.xy, linear.xy) > (FLICK_THRESH * FLICK_THRESH)) {
                timer = 1.0;
            }
            fragColor = vec4(timer, 0.0, 0.0, 1.0);
            return;
        }

        // ---- px 17..24: Planetary Gear Phase Integrators ----
        if (px >= 17.0 && px <= 24.0) {
            float gIdx = px - 17.0;
            float prevPhase = unpack16(slot(px).rg);

            // Orbit axle (plane normal); vertical disk, outliers swing left-right
            float tilt = gearTilt(gIdx);
            vec3 gearAxle = normalize(vec3(cos(tilt), 0.0, -sin(tilt)));

            // Mechanical gearing ratios and Keplerian idle speeds
            float gearRatio = 4.0 / (gIdx * 0.6 + 1.0);
            float idleRate  = (0.04 / sqrt(gIdx + 1.0)) / FPS;

            // Torque projection onto gear axle
            float mechanicalDrive = dot(omega, gearAxle) * (gearRatio / FPS);

            float nextPhase = fract(prevPhase + idleRate + mechanicalDrive);
            fragColor = vec4(pack16(nextPhase), 0.0, 1.0);
            return;
        }

        // Output master physics slots
        vec2 o;
        if      (px <  1.0) o = encS(q.x, 1.0);
        else if (px <  2.0) o = encS(q.y, 1.0);
        else if (px <  3.0) o = encS(q.z, 1.0);
        else if (px <  4.0) o = encS(q.w, 1.0);
        else if (px <  5.0) o = encS(omega.x, OMEGA_MAX);
        else if (px <  6.0) o = encS(omega.y, OMEGA_MAX);
        else if (px <  7.0) o = encS(omega.z, OMEGA_MAX);
        else if (px <  8.0) o = encS(fling.x, OMEGA_MAX);
        else if (px <  9.0) o = encS(fling.y, OMEGA_MAX);
        else if (px < 10.0) o = encS(fling.z, OMEGA_MAX);
        else if (px < 11.0) o = pack16(lastTouch.x / surfaceResolution.x);
        else                o = pack16(lastTouch.y / surfaceResolution.y);
        fragColor = vec4(o, 0.0, 1.0);
        return;
    }

    // ------------------------------------------------------------ Row 1: Direct Band Passthrough
    if (gl_FragCoord.y < 2.0) {
        float b = floor(gl_FragCoord.x);
        if (b >= N_BANDS) discard;
        float clean = texture(bands, vec2((b + 0.5) / N_BANDS, 0.5)).r;
        fragColor = vec4(clean, clean, clean, 1.0);
        return;
    }

    discard;
}

// ==================== Pass 2: Image ====================
==image==
uniform sampler2D state;
uniform vec2      resolution;
uniform vec2      stateRes;
uniform float     time;
uniform bool      screenLocked;

float qComp(float px) {
    return decS(texture(state, vec2((px + 0.5) / stateRes.x, 0.5 / stateRes.y)).rg, 1.0);
}

vec4 loadQuat() {
    return normalize(vec4(qComp(0.0), qComp(1.0), qComp(2.0), qComp(3.0)));
}

float fetchCleanBand(float b) {
    return texture(state, vec2((b + 0.5) / stateRes.x, 1.5 / stateRes.y)).r;
}

float fetchGearPhase(int i) {
    return unpack16(texture(state, vec2((17.0 + float(i) + 0.5) / stateRes.x, 0.5 / stateRes.y)).rg);
}

void main() {
    vec2 uv = (-resolution.xy + 2.0 * gl_FragCoord.xy) / resolution.y;
    vec2 center = SENSOR_POS;

    vec3 ro = vec3(0.0, 0.0, -2.4 / BALL_ZOOM);
    vec3 rd = normalize(vec3(uv - center, 1.8));

    // Audio reactivity
    float bass = fetchCleanBand(2.0);

    // Transform camera ray into trackball's local coordinate frame via transpose()
    mat3 trackballRot = quatToMat3(loadQuat());
    mat3 invRot = transpose(trackballRot);

    vec3 roL = invRot * (ro - vec3(SENSOR_POS, 0.0));
    vec3 rdL = invRot * rd;

    vec3 col = vec3(0.005, 0.007, 0.012); // Deep vacuum background
    vec2 outerHit = intersectSphere(roL, rdL, SPHERE_RADIUS);

    if (outerHit.x > 0.0) {
        float minT = 1e5;
        vec3 sceneCol = vec3(0.0);

        // 1. Central Core / Sun (limb darkening + granulation + corona)
        float sunR = 0.12 * (1.0 + bass * 0.12 + 0.02 * sin(time * 1.6));
        vec2 sunHit = intersectSphere(roL, rdL, sunR);
        if (sunHit.x > 0.0) {
            minT = sunHit.x;
            vec3 pSun = roL + rdL * sunHit.x;
            vec3 nSun = normalize(pSun);
            float coreGlow = clamp(dot(-rdL, nSun), 0.0, 1.0);
            float gran = hash21(nSun.xy * 60.0 + nSun.z * 40.0 + floor(time * 3.0)) * 0.06;
            sceneCol = mix(vec3(1.0, 0.45, 0.08), vec3(1.0, 0.88, 0.5), pow(coreGlow, 3.0));
            sceneCol *= (1.7 + bass * 2.4) * (1.0 + gran);
        } else {
            // corona: rays that miss the disk but pass close to the sun
            float tC = -dot(roL, rdL);
            if (tC > 0.0) {
                float b = length(roL + rdL * tC);
                col += vec3(1.0, 0.66, 0.26) * exp(-max(b - sunR, 0.0) * 20.0) * (0.3 + bass * 0.5);
            }
        }



        // 2. Geared Orbiting Spheres + a ringed gas giant
        for (int i = 0; i < N_GEARS; i++) {
            float fi = float(i);
            float rOrbit = 0.22 + fi * 0.078;
            float theta = fract(fetchGearPhase(i) + gearPhaseOffset(fi)) * TAU;

            float tilt = gearTilt(fi);
            vec3 uAxis = vec3(0.0, 1.0, 0.0);
            vec3 vAxis = vec3(sin(tilt), 0.0, cos(tilt));
            vec3 pPos = (uAxis * cos(theta) + vAxis * sin(theta)) * rOrbit;
            float pR = planetRadius(fi);

            // planet sphere
            vec2 pHit = intersectSphere(roL - pPos, rdL, pR);
            if (pHit.x > 0.0 && pHit.x < minT) {
                minT = pHit.x;
                vec3 nP = normalize((roL + rdL * pHit.x) - pPos);
                vec3 lightDir = -normalize(pPos);
                float diff = clamp(dot(nP, lightDir), 0.04, 1.0);
                vec3 base;
                if (isGasGiant(fi) > 0.5) {
                    base = gasGiantColor(nP, fi, time);
                } else {
                    float bandVal = fetchCleanBand(fi * 13.0);
                    base = mix(vec3(0.32, 0.55, 0.72), vec3(0.75, 0.34, 0.42), fi / 7.0);
                    base *= (1.0 + bandVal * 1.4);
                }
                sceneCol = base * (0.08 + diff);
            }

            // ring around the ringed gas giant
            if (hasRing(fi) > 0.5) {
                vec3 ringN = normalize(vec3(0.48, 0.48, 0.73));
                float denomR = dot(rdL, ringN);
                if (abs(denomR) > 1e-4) {
                    float tR = dot(pPos - roL, ringN) / denomR;
                    if (tR > 0.0 && tR < minT) {
                        vec3 rel = (roL + rdL * tR) - pPos;
                        float rad = length(rel - ringN * dot(rel, ringN));
                        float rIn = pR * 1.30;
                        float rOut = pR * 2.50;
                        float ringMask = smoothstep(rIn, rIn + 0.006, rad)
                                       * (1.0 - smoothstep(rOut - 0.006, rOut, rad));
                        float cass = smoothstep(pR * 1.72, pR * 1.82, rad)
                                   - smoothstep(pR * 1.86, pR * 1.96, rad);
                        ringMask *= (1.0 - cass);
                        if (ringMask > 0.002) {
                            minT = tR;
                            float l = clamp(abs(dot(ringN, normalize(-pPos))), 0.12, 1.0);
                            sceneCol = vec3(0.72, 0.63, 0.49) * l * (0.35 + 0.65 * ringMask);
                        }
                    }
                }
            }
        }

        // 3. Invisible Outer Shell / Subtle Fresnel Enclosure
        vec3 pOuter = roL + rdL * outerHit.x;
        float fresnel = pow(1.0 - clamp(dot(-rdL, normalize(pOuter)), 0.0, 1.0), 3.5);
        vec3 glassRim = vec3(0.04, 0.15, 0.25) * fresnel;

        col = (minT < 1e5) ? sceneCol : col;
        col += glassRim;
    }

    if (screenLocked) {
        col *= smoothstep(0.08, 0.16, length(uv - SENSOR_POS));
    }

    fragColor = vec4(col, 1.0);
}
