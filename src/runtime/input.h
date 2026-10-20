#pragma once

/* DirectInput, steered (input.c). */
void input_init(int headless);
int input_arg(int argc, char** argv, int i);      /* --key; how many argv entries it took, 0 if none */
void input_shim_DirectInputCreateA(void);          /* native32 shim for DINPUT!DirectInputCreateA */
