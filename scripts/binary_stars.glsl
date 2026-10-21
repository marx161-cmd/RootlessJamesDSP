// Binary star system inside an invisible, freely-rotatable containment sphere.
// Swipe/fling to spin the whole thing (trackball quaternion, ported from the
// storm shader); the donor rips a hot gas stream into the compact companion.
// Frozen photo backdrop (swap `backdrop.bin`). All analytic -- no raymarch.

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

#define EXT_TEXTURE_backdrop rgba8 1280 2856

uniform vec2 resolution;
uniform float time;
uniform sampler2D backdrop;

#define PI 3.141592653589793

// ---- quaternion / state packing helpers ----
vec2 pack16(float v) {
    float x = floor(clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
    float hi = floor(x / 256.0);
    return vec2(hi, x - hi * 256.0) / 255.0;
}
float unpack16(vec2 c) {
    vec2 b = floor(c * 255.0 + 0.5);
    return (b.x * 256.0 + b.y) / 65535.0;
}
vec2 encS(float v, float range) {
    float x = floor(clamp(v / range, -1.0, 1.0) * 32767.0 + 32767.5);
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
    float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
    float yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    return mat3(
        1.0 - (yy + zz), xy + wz, xz - wy,
        xy - wz, 1.0 - (xx + zz), yz + wx,
        xz + wy, yz - wx, 1.0 - (xx + yy)
    );
}

// ---- binary helpers ----
vec3 blackbody(float t) {
    vec3 c;
    if (t <= 66.0) {
        c.r = 1.0;
        c.g = clamp(0.3900815787 * log(t) - 0.6318414438, 0.0, 1.0);
        if (t <= 19.0) c.b = 0.0;
        else c.b = clamp(0.5432067866 * log(t - 10.0) - 1.1962540891, 0.0, 1.0);
    } else {
        c.r = clamp(1.2929361861 * pow(t - 60.0, -0.1332047592), 0.0, 1.0);
        c.g = clamp(1.1298908609 * pow(t - 60.0, -0.0755148492), 0.0, 1.0);
        c.b = 1.0;
    }
    return c;
}
vec3 rotX(vec3 p, float a) {
    float c = cos(a), s = sin(a);
    return vec3(p.x, c * p.y - s * p.z, s * p.y + c * p.z);
}
float rayEllipsoid(vec3 ro, vec3 rd, vec3 ce, vec3 e1, vec3 e2, vec3 e3, vec3 ax) {
    vec3 p = ro - ce;
    vec3 pl = vec3(dot(p, e1), dot(p, e2), dot(p, e3));
    vec3 dl = vec3(dot(rd, e1), dot(rd, e2), dot(rd, e3));
    vec3 ia = 1.0 / (ax * ax);
    float A = dot(dl * dl, ia);
    float B = 2.0 * dot(pl * dl, ia);
    float C = dot(pl * pl, ia) - 1.0;
    float disc = B * B - 4.0 * A * C;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t0 = (-B - s) / (2.0 * A);
    float t1 = (-B + s) / (2.0 * A);
    if (t0 > 0.0) return t0;
    if (t1 > 0.0) return t1;
    return -1.0;
}

// ==================== Pass 1: state (trackball) ====================
==state 32x1==
uniform sampler2D state;
uniform vec2 surfaceResolution;
uniform vec2 touch;
uniform int pointerCount;
uniform bool screenLocked;
uniform vec3 linear;
uniform vec3 gravity;

#define OMEGA_MAX           0.6
#define TRACKBALL_GAIN      2.6
#define FLING_DECAY         0.92
#define FRICTION            0.965
#define GRAVITY_TORQUE_GAIN 0.00022
#define GRAV_TERMINAL_SPEED 0.08
#define RESTORE_SPEED_LOCK  0.35
#define FLICK_THRESH        4.5
#define VIRTUAL_KICK_SPEED  0.22
#define COOLDOWN_FRAMES     22.0
#define TRACKBALL_RADIUS    0.273

vec4 slot(float px) {
    return texture2D(state, vec2((px + 0.5) / resolution.x, 0.5 / resolution.y));
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
    vec2 c = uv / TRACKBALL_RADIUS;
    float r2 = dot(c, c);
    if (r2 <= 1.0) return vec3(c, -sqrt(1.0 - r2));
    return vec3(normalize(c), 0.0);
}

void main() {
    if (gl_FragCoord.y >= 1.0) discard;
    float px = floor(gl_FragCoord.x);
    if (px > 16.0) discard;

    vec4 q = vec4(rdS(0.0, 1.0), rdS(1.0, 1.0), rdS(2.0, 1.0), rdS(3.0, 1.0));
    vec3 omega = vec3(rdS(4.0, OMEGA_MAX), rdS(5.0, OMEGA_MAX), rdS(6.0, OMEGA_MAX));
    vec3 fling = vec3(rdS(7.0, OMEGA_MAX), rdS(8.0, OMEGA_MAX), rdS(9.0, OMEGA_MAX));
    vec2 lastTouch = vec2(unpack16(slot(10.0).rg), unpack16(slot(11.0).rg)) * surfaceResolution;
    bool wasTouching = (lastTouch.x + lastTouch.y) > 0.5;

    float qq = dot(q, q);
    if (!(qq > 0.5 && qq < 1.5)) { q = vec4(0.0, 0.0, 0.0, 1.0); omega = vec3(0.0); fling = vec3(0.0); wasTouching = false; }
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
        }
    }

    // shove / flick impulse
    float flickTimer = slot(16.0).r;
    float shoveSq = dot(linear.xy, linear.xy);
    if (flickTimer <= (1.0 / COOLDOWN_FRAMES) && shoveSq > (FLICK_THRESH * FLICK_THRESH)) {
        vec2 sdir = linear.xy * inversesqrt(shoveSq);
        omega += vec3(sdir.y, -sdir.x, 0.0) * VIRTUAL_KICK_SPEED;
    }

    // 3D gravity torque
    vec3 gravTorque = cross(vec3(0.0, 0.0, 1.0), gravity);
    float curSpd = length(omega);
    if (curSpd < GRAV_TERMINAL_SPEED) omega += gravTorque * GRAVITY_TORQUE_GAIN;

    float spd = length(omega);
    if (spd > OMEGA_MAX) omega *= OMEGA_MAX / spd;

    float ang = length(omega);
    if (ang > 1e-6) {
        vec4 dq = vec4(omega / ang * sin(ang * 0.5), cos(ang * 0.5));
        q = normalize(quatMul(dq, q));
    }

    vec2 o;
    if (px < 1.0) o = encS(q.x, 1.0);
    else if (px < 2.0) o = encS(q.y, 1.0);
    else if (px < 3.0) o = encS(q.z, 1.0);
    else if (px < 4.0) o = encS(q.w, 1.0);
    else if (px < 5.0) o = encS(omega.x, OMEGA_MAX);
    else if (px < 6.0) o = encS(omega.y, OMEGA_MAX);
    else if (px < 7.0) o = encS(omega.z, OMEGA_MAX);
    else if (px < 8.0) o = encS(fling.x, OMEGA_MAX);
    else if (px < 9.0) o = encS(fling.y, OMEGA_MAX);
    else if (px < 10.0) o = encS(fling.z, OMEGA_MAX);
    else if (px < 11.0) o = pack16(lastTouch.x / surfaceResolution.x);
    else if (px < 12.0) o = pack16(lastTouch.y / surfaceResolution.y);
    else if (px < 16.0) o = vec2(0.0);
    else {
        float timer = slot(16.0).r;
        timer = max(0.0, timer - (1.0 / COOLDOWN_FRAMES));
        if (timer <= 0.0 && dot(linear.xy, linear.xy) > (FLICK_THRESH * FLICK_THRESH)) timer = 1.0;
        o = vec2(timer, 0.0);
    }
    gl_FragColor = vec4(o, 0.0, 1.0);
}

// ==================== Pass 2: image ====================
==image==
uniform sampler2D state;
uniform vec2 stateRes;

float qComp(float px) {
    return decS(texture2D(state, vec2((px + 0.5) / stateRes.x, 0.5 / stateRes.y)).rg, 1.0);
}
vec4 loadQuat() {
    return normalize(vec4(qComp(0.0), qComp(1.0), qComp(2.0), qComp(3.0)));
}

void main() {
    vec2 uv = (-resolution.xy + 2.0 * gl_FragCoord.xy) / resolution.y;

    // frozen photo backdrop
    vec2 buv = vec2(gl_FragCoord.x / resolution.x, 1.0 - gl_FragCoord.y / resolution.y);
    vec3 col = texture2D(backdrop, buv).rgb;

    // fixed camera
    float camDist = 5.0;
    float focal = 1.7;
    vec3 ro = vec3(0.0, 0.0, -camDist);
    vec3 rd = normalize(vec3(uv, focal));

    // rotate the whole binary scene by the trackball quaternion (inverse)
    vec4 quat = loadQuat();
    mat3 Rinv = quatToMat3(vec4(-quat.x, -quat.y, -quat.z, quat.w)); // conjugate = inverse
    vec3 rol = ro * Rinv;
    vec3 rdl = normalize(rd * Rinv);

    // binary parameters (local frame)
    float R1 = 1.0, T1 = 3800.0;   // donor
    float R2 = 0.24, T2 = 22000.0; // compact
    float dsep = 1.35, q = 0.4;
    float theta = time * 0.7;
    float incl = 0.68;

    vec3 bdir = vec3(cos(theta), sin(theta), 0.0);
    float x1 = -dsep * q / (1.0 + q);
    float x2 = dsep / (1.0 + q);

    vec3 c1 = rotX(bdir * x1, incl);
    vec3 c2 = rotX(bdir * x2, incl);
    vec3 eA = rotX(bdir, incl);
    vec3 planeN = rotX(vec3(0.0, 0.0, 1.0), incl);
    vec3 eB = normalize(cross(eA, planeN));
    vec3 eC = cross(eA, eB);

    float e1 = 0.30, e2 = 0.12;
    vec3 ax1 = vec3(R1 * (1.0 + e1), R1 * (1.0 - 0.5 * e1), R1 * (1.0 - 0.5 * e1));
    vec3 ax2 = vec3(R2 * (1.0 + e2), R2 * (1.0 - 0.5 * e2), R2 * (1.0 - 0.5 * e2));

    float t1 = rayEllipsoid(rol, rdl, c1, eA, eB, eC, ax1);
    float t2 = rayEllipsoid(rol, rdl, c2, -eA, eB, eC, ax2);

    // accretion disk glow (analytic annulus)
    vec3 glow = vec3(0.0);
    float denom = dot(rdl, planeN);
    if (abs(denom) > 1e-4) {
        float tp = dot(c2 - rol, planeN) / denom;
        if (tp > 0.0) {
            vec3 hit = rol + rdl * tp;
            vec3 rel = hit - c2;
            vec2 relp = vec2(dot(rel, eA), dot(rel, eB));
            float r = length(relp);
            float ring = smoothstep(R2 * 1.4, R2 * 2.2, r) * (1.0 - smoothstep(R2 * 3.6, R2 * 5.0, r));
            glow += blackbody(95.0) * ring * 3.0;
        }
    }

    // spiral accretion stream
    const float STREAM_TURNS = 3.0;
    const int STREAM_STEPS = 28;
    float rL1 = 0.72, rMin = R2 * 1.5;
    float streamMin = 1e9;
    for (int i = 0; i < STREAM_STEPS; i++) {
        float f = float(i) / float(STREAM_STEPS - 1);
        float phi = f * STREAM_TURNS * 2.0 * PI;
        float rr = mix(rL1, rMin, f);
        vec3 p = c2 + eA * (rr * cos(phi)) + eB * (rr * sin(phi));
        vec3 d = p - rol;
        float t = dot(d, rdl);
        float perp = length(d - rdl * max(t, 0.0));
        streamMin = min(streamMin, perp);
    }
    glow += vec3(1.0, 0.85, 0.6) * exp(-streamMin * streamMin * 220.0) * 1.4;

    // nearest star hit
    float tHit = -1.0;
    float which = 0.0;
    if (t1 > 0.0) { tHit = t1; which = 1.0; }
    if (t2 > 0.0 && (tHit < 0.0 || t2 < tHit)) { tHit = t2; which = 2.0; }

    if (tHit > 0.0) {
        vec3 ce = (which > 1.5) ? c2 : c1;
        vec3 el = (which > 1.5) ? -eA : eA;
        vec3 ax = (which > 1.5) ? ax2 : ax1;
        float T = (which > 1.5) ? T2 : T1;
        vec3 p = rol + rdl * tHit;
        vec3 pl = vec3(dot(p - ce, el), dot(p - ce, eB), dot(p - ce, eC));
        vec3 nL = pl / (ax * ax);
        vec3 n = normalize(el * nL.x + eB * nL.y + eC * nL.z);
        float mu = clamp(dot(n, -rdl), 0.0, 1.0);
        float limb = mix(0.18, 1.0, pow(mu, 0.6));
        col = blackbody(T / 100.0) * limb * 1.6;

        if (which > 1.5) {
            float sd = dot(n, -eA);
            col += vec3(1.0, 0.92, 0.78) * exp(-9.0 * max(0.0, 1.0 - sd)) * 2.8;
        }
    } else {
        col += glow;
    }

    col = col / (1.0 + col);
    gl_FragColor = vec4(col, 1.0);
}
