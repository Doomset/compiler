#include <float>
#include <console>

enum E_ITEM { E_ID, Float:E_POS[3], E_NAME[8] }
#define NEG -2

new const Float:gA[3][4] = {
    {0.1480000,0.0334300,-1.3362450,1.0000000},
    {-0.0000000, - 0.5 ,-7.25e2, 1.0e-3},
    {1.5, -1.5, 2.0, -(3.0)}
};
new gB[] = { 1, -2, 0x10, -0x10, 0b101, -0b11, 1_000, -2_000, 'a', -'b', NEG, -(NEG), };
new gC[6] = { 1, 2, ... };
new gD[5] = { -1, -3, ... };
new Float:gE[4] = { 1.0, 2.0, ... };
new gF[2][E_ITEM] = {
    { 1, { 1.0, -2.0, 3.0 }, "one" },
    { -2, { -1.5, 2.5, -3.5 }, "two" }
};
new Float:gG[3] = { Float:1, -1.0, 2.0 };
new gH[4] = { 1, 2 + 3, -4 * 2, ~5 };
new Float:gI[3] = { 1.0,
    -2.0
    , 3.0 };
new gJ[3] = { -1
, -2, -3 };
new gK[] = { -0, -2147483647, 2147483647, -1 };
new Float:gL[2] = { -1.0 , -0.0 };
new gM[3] = { 1, 2, 3 }, gN = -5, Float:gO = -2.5;
static const gP[][] = { "ab", "cd" };
new bool:gQ[2] = { true, false };
new Float:gR[2] = { 1, -2 };
new gS[2] = { 1.0, -2.0 };

main()
{
    new loc[3] = { -1, 2, -3 };
    new Float:floc[2] = { -1.0, 2.0 };
    printf("%d %d %f %d %f %d %f %d %d %f %d %d %d %d %f %d %d %f %d %d %d",
        gA[1][1], gB[3], gE[3], gD[4], gF[1][E_POS][2], gH[2], gI[1], gJ[1], gK[1], gL[1], gM[2], gN,
        loc[2], gC[5], floc[0], gP[1][0], gQ[0], gR[1], gS[1], _:gO, gF[0][E_NAME][0]);
}
