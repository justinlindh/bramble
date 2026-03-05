#include "sim_random.h"

void pcg32_seed(pcg32_state_t* rng, uint64_t seed) {
    rng->state = 0;
    rng->inc = (seed << 1) | 1;
    pcg32_random(rng);
    rng->state += seed;
    pcg32_random(rng);
}

uint32_t pcg32_random(pcg32_state_t* rng) {
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

uint32_t pcg32_range(pcg32_state_t* rng, uint32_t min, uint32_t max) {
    uint32_t range = max - min;
    if (range == 0)
        return min;
    return min + (pcg32_random(rng) % range);
}

float pcg32_float(pcg32_state_t* rng) { return (float)pcg32_random(rng) / (float)0xFFFFFFFFU; }
