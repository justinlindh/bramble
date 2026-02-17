#ifndef SIM_RANDOM_H
#define SIM_RANDOM_H

#include <stdint.h>

/* PCG32 random number generator state */
typedef struct {
    uint64_t state;
    uint64_t inc;
} pcg32_state_t;

void pcg32_seed(pcg32_state_t *rng, uint64_t seed);
uint32_t pcg32_random(pcg32_state_t *rng);
uint32_t pcg32_range(pcg32_state_t *rng, uint32_t min, uint32_t max);
float pcg32_float(pcg32_state_t *rng);  /* Returns [0.0, 1.0) */

#endif /* SIM_RANDOM_H */
