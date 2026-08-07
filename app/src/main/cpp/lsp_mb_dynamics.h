#ifndef LSP_MB_DYNAMICS_H_
#define LSP_MB_DYNAMICS_H_

#ifdef __cplusplus
extern "C" {
#endif

void mb_dyn_init(float sample_rate, unsigned num_bands);
void mb_dyn_destroy(void);

void mb_dyn_set_comp(unsigned band, int enabled,
                     float threshold, float ratio, float knee,
                     float attack, float release, float hold);
void mb_dyn_set_gate(unsigned band, int enabled,
                     float open_db, float close_db,
                     float attack, float release, float hold, float reduction);
void mb_dyn_set_eq(unsigned band, float gain_db);

void mb_dyn_process(float *left, float *right, unsigned n);
unsigned mb_dyn_num_bands(void);
float mb_dyn_get_band_energy(unsigned band);

#ifdef __cplusplus
}
#endif

#endif
