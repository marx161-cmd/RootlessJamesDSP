#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

// Sphere made of audio-reactive hexagons (test).
// Each hex = one (pitch-class x octave) cell; brightness = clean[cell].
// Pitch class wraps around longitude, octave runs pole-to-pole (latitude).
// Reads clean[0..107] from reactive.lua (already harmonic-subtracted,
// flatness-gated, settled). No rotation state needed -- auto-spins slowly.

uniform vec2  resolution;
uniform float time;
uniform float clean[108];

// --- pointy-top axial hex grid (from shader-backdrop common.glsl) ---
vec2 hexRound(vec2 axial) {
    float x = axial.x, z = axial.y, y = -x - z;
    float rx = floor(x + 0.5), ry = floor(y + 0.5), rz = floor(z + 0.5);
    float xD = abs(rx - x), yD = abs(ry - y), zD = abs(rz - z);
    if (xD > yD && xD > zD) { rx = -ry - rz; }
    else if (yD > zD) { ry = -rx - rz; }
    else { rz = -rx - ry; }
    return vec2(rx, rz);
}
vec2 hexCenter(vec2 c, float s) { return s * vec2(1.7320508*c.x + 0.8660254*c.y, 1.5*c.y); }
void hexInfo(vec2 p, float s, out vec2 id, out vec2 local) {
    float q = (0.57735027*p.x - 0.33333333*p.y) / s;
    float r = (0.66666667*p.y) / s;
    id = hexRound(vec2(q, r));
    local = p - hexCenter(id, s);
}

// --- pointy-top hexagon SDF (from shader-backdrop image.glsl) ---
float hexEdgeDist(vec2 p, float inradius) {
    float c30 = 0.8660254, s30 = 0.5;
    p = vec2(c30*p.x - s30*p.y, s30*p.x + c30*p.y);
    vec3 k = vec3(-0.8660254, 0.5, 0.57735027);
    vec2 q = abs(p);
    q -= 2.0*min(dot(k.xy, q), 0.0)*k.xy;
    vec2 d = vec2(clamp(q.x, -k.z*inradius, k.z*inradius), inradius);
    return length(q - d) * sign(q.y - inradius);
}

vec2 intersectSphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r*r;
    float h = b*b - c;
    if (h < 0.0) return vec2(-1.0);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

void main() {
    vec2 uv = (-resolution.xy + 2.0*gl_FragCoord.xy) / resolution.y;
    vec3 ro = vec3(0.0, 0.0, -2.4);
    vec3 rd = normalize(vec3(uv, 1.8));
    vec2 hit = intersectSphere(ro, rd, 0.88);
    if (hit.x < 0.0) { gl_FragColor = vec4(0.01, 0.01, 0.02, 1.0); return; }

    vec3 n0 = normalize(ro + rd*hit.x);
    // slow spin around Y
    float a = time * 0.35;
    vec3 n;
    n.x = n0.x*cos(a) + n0.z*sin(a);
    n.y = n0.y;
    n.z = -n0.x*sin(a) + n0.z*cos(a);

    float theta = acos(clamp(n.z, -1.0, 1.0));
    float phi = atan(n.y, n.x);
    if (phi < 0.0) phi += 6.283185307;

    // 12 pitch classes around, 9 octave rows pole-to-pole (0.866 row pitch)
    float pX = phi / 6.283185307 * 12.0;
    float pY = theta / 3.141592654 * 7.79;
    vec2 id; vec2 local;
    hexInfo(vec2(pX, pY), 0.57735027, id, local);
    int col = int(mod(id.x, 12.0));
    int row = int(clamp(id.y, 0.0, 8.0));
    int idx = row*12 + col;

    float v = 0.0;
    if (idx == 0) v = clean[0];
    else if (idx == 1) v = clean[1];
    else if (idx == 2) v = clean[2];
    else if (idx == 3) v = clean[3];
    else if (idx == 4) v = clean[4];
    else if (idx == 5) v = clean[5];
    else if (idx == 6) v = clean[6];
    else if (idx == 7) v = clean[7];
    else if (idx == 8) v = clean[8];
    else if (idx == 9) v = clean[9];
    else if (idx == 10) v = clean[10];
    else if (idx == 11) v = clean[11];
    else if (idx == 12) v = clean[12];
    else if (idx == 13) v = clean[13];
    else if (idx == 14) v = clean[14];
    else if (idx == 15) v = clean[15];
    else if (idx == 16) v = clean[16];
    else if (idx == 17) v = clean[17];
    else if (idx == 18) v = clean[18];
    else if (idx == 19) v = clean[19];
    else if (idx == 20) v = clean[20];
    else if (idx == 21) v = clean[21];
    else if (idx == 22) v = clean[22];
    else if (idx == 23) v = clean[23];
    else if (idx == 24) v = clean[24];
    else if (idx == 25) v = clean[25];
    else if (idx == 26) v = clean[26];
    else if (idx == 27) v = clean[27];
    else if (idx == 28) v = clean[28];
    else if (idx == 29) v = clean[29];
    else if (idx == 30) v = clean[30];
    else if (idx == 31) v = clean[31];
    else if (idx == 32) v = clean[32];
    else if (idx == 33) v = clean[33];
    else if (idx == 34) v = clean[34];
    else if (idx == 35) v = clean[35];
    else if (idx == 36) v = clean[36];
    else if (idx == 37) v = clean[37];
    else if (idx == 38) v = clean[38];
    else if (idx == 39) v = clean[39];
    else if (idx == 40) v = clean[40];
    else if (idx == 41) v = clean[41];
    else if (idx == 42) v = clean[42];
    else if (idx == 43) v = clean[43];
    else if (idx == 44) v = clean[44];
    else if (idx == 45) v = clean[45];
    else if (idx == 46) v = clean[46];
    else if (idx == 47) v = clean[47];
    else if (idx == 48) v = clean[48];
    else if (idx == 49) v = clean[49];
    else if (idx == 50) v = clean[50];
    else if (idx == 51) v = clean[51];
    else if (idx == 52) v = clean[52];
    else if (idx == 53) v = clean[53];
    else if (idx == 54) v = clean[54];
    else if (idx == 55) v = clean[55];
    else if (idx == 56) v = clean[56];
    else if (idx == 57) v = clean[57];
    else if (idx == 58) v = clean[58];
    else if (idx == 59) v = clean[59];
    else if (idx == 60) v = clean[60];
    else if (idx == 61) v = clean[61];
    else if (idx == 62) v = clean[62];
    else if (idx == 63) v = clean[63];
    else if (idx == 64) v = clean[64];
    else if (idx == 65) v = clean[65];
    else if (idx == 66) v = clean[66];
    else if (idx == 67) v = clean[67];
    else if (idx == 68) v = clean[68];
    else if (idx == 69) v = clean[69];
    else if (idx == 70) v = clean[70];
    else if (idx == 71) v = clean[71];
    else if (idx == 72) v = clean[72];
    else if (idx == 73) v = clean[73];
    else if (idx == 74) v = clean[74];
    else if (idx == 75) v = clean[75];
    else if (idx == 76) v = clean[76];
    else if (idx == 77) v = clean[77];
    else if (idx == 78) v = clean[78];
    else if (idx == 79) v = clean[79];
    else if (idx == 80) v = clean[80];
    else if (idx == 81) v = clean[81];
    else if (idx == 82) v = clean[82];
    else if (idx == 83) v = clean[83];
    else if (idx == 84) v = clean[84];
    else if (idx == 85) v = clean[85];
    else if (idx == 86) v = clean[86];
    else if (idx == 87) v = clean[87];
    else if (idx == 88) v = clean[88];
    else if (idx == 89) v = clean[89];
    else if (idx == 90) v = clean[90];
    else if (idx == 91) v = clean[91];
    else if (idx == 92) v = clean[92];
    else if (idx == 93) v = clean[93];
    else if (idx == 94) v = clean[94];
    else if (idx == 95) v = clean[95];
    else if (idx == 96) v = clean[96];
    else if (idx == 97) v = clean[97];
    else if (idx == 98) v = clean[98];
    else if (idx == 99) v = clean[99];
    else if (idx == 100) v = clean[100];
    else if (idx == 101) v = clean[101];
    else if (idx == 102) v = clean[102];
    else if (idx == 103) v = clean[103];
    else if (idx == 104) v = clean[104];
    else if (idx == 105) v = clean[105];
    else if (idx == 106) v = clean[106];
    else if (idx == 107) v = clean[107];

    // hexagon fill + dark rim at the shared edges
    float inr = 0.44;
    float d = hexEdgeDist(local, inr);
    float aa = smoothstep(0.02, 0.0, d);
    float rim = smoothstep(-0.05, -0.01, d);

    vec3 c = mix(vec3(0.02,0.02,0.05), vec3(0.0,0.5,1.0), smoothstep(0.0,0.5,v));
    c = mix(c, vec3(1.0,0.85,0.2), smoothstep(0.5,0.95,v));
    c = mix(c, vec3(1.0), smoothstep(0.95,1.0,v));
    c = mix(c, vec3(0.04,0.04,0.07), rim);

    vec3 surf = vec3(0.035, 0.04, 0.06);
    vec3 outCol = mix(surf, c, aa);

    // fresnel rim light so the sphere reads as a ball
    float fres = pow(1.0 - max(0.0, -dot(rd, n0)), 2.0);
    outCol += vec3(0.14, 0.18, 0.32) * fres;

    gl_FragColor = vec4(outCol, 1.0);
}
