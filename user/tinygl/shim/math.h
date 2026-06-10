// Hardware where AArch64 has an instruction (sqrt/fabs/floor via builtins),
// small series where it doesn't (sin/cos/pow) -- see tgl_rt.c.
#pragma once
#define sqrt(x)  __builtin_sqrt(x)
#define fabs(x)  __builtin_fabs(x)
#define floor(x) __builtin_floor(x)
#define ceil(x)  __builtin_ceil(x)
double sin(double x);
double cos(double x);
double pow(double x, double y);
#define M_PI 3.14159265358979323846
