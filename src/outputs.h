#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <stdbool.h>
#include <stdint.h>

void outputs_init(void);
void gate_set(int track /*0..2*/, bool active);
void cv_mod16(int track /*0..2*/, uint16_t level);   /* 0..65535 */
void cv_pitch16(int track /*0..2*/, uint16_t level); /* 0..65535 */
void outputs_all_off(void);

#endif
