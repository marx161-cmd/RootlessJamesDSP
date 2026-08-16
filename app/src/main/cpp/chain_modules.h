// chain_modules.h — C interface for chain modules and AutoGain/Gate

#ifndef CHAIN_MODULES_H
#define CHAIN_MODULES_H

#ifdef __cplusplus
extern "C" {
#endif

void chain_register(const char *name, void (*fn)(float*,float*,unsigned), int *toggle);
void chain_reset(void);
int  chain_set_order(int *indices, int len);
void chain_process(float *L, float *R, unsigned n);

void gate_init(void *g, float open_db, float close_db, float attack_s,
               float release_s, float hold_s, float fs);
void gate_process(void *g, float *L, float *R, unsigned n);
void autogain_process(float *L, float *R, unsigned n);

// Current applied AutoGain correction, in dB (0 when disabled). Live value —
// reads the same integrator autogain_process() updates each block.
float autogain_get_gain_db(void);

#ifdef __cplusplus
}
#endif

#endif
