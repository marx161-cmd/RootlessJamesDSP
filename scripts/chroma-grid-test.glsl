#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

// Chroma band-meter test grid: 9 octave rows x 12 semitone columns.
// Each cell brightness = its CLEANED band energy (harmonic-subtracted,
// flatness-gated, settled). Row 0 (bottom) = A0 27.5 Hz, row 8 (top) =
// ~13.3 kHz; col 0 (left) = A, col 11 (right) = G#.
// Switch `clean` -> `bands` below to see the RAW values instead.

uniform vec2  resolution;

uniform float clean[108];

void main() {
    vec2 uv = gl_FragCoord.xy / resolution.xy;
    int row = clamp(int(uv.y * 9.0), 0, 8);
    int col = clamp(int(uv.x * 12.0), 0, 11);
    int idx = row * 12 + col;

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

    vec2 cellUv = fract(uv * vec2(12.0, 9.0));
    float edge = max(step(0.94, cellUv.x), step(0.94, cellUv.y));

    vec3 c = mix(vec3(0.02, 0.02, 0.04), vec3(0.0, 0.5, 1.0), smoothstep(0.0, 0.5, v));
    c = mix(c, vec3(1.0, 0.85, 0.2), smoothstep(0.5, 0.95, v));
    c = mix(c, vec3(1.0), smoothstep(0.95, 1.0, v));
    c = mix(c, vec3(0.13, 0.13, 0.17), edge);

    gl_FragColor = vec4(c, 1.0);
}
