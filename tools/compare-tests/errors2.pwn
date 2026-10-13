#include <float>
#define NEG -2
new const Float:gA[2] = { -1.5*2.0, 2.0+1.0 };
new gB[2] = { NEG, -NEG };
main() { return _:gA[0] + gB[1]; }
