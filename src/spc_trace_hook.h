#ifndef SPC_TRACE_HOOK_H
#define SPC_TRACE_HOOK_H

extern void spc_trace_dsp_write(int time, int reg, int value);

#define SPC_DSP_WRITE_HOOK(time, reg, value) spc_trace_dsp_write((time), (reg), (value))

#endif
