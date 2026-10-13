#pragma rational Fixed(3)
new Fixed:gA[4] = { 1.5, -1.5, -0.001, 2.12345 };
new Fixed:gB[2] = { -1.25 , 3.0 };
main() { return _:gA[1] + _:gA[3] + _:gB[0]; }
