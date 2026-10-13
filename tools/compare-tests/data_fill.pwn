// Data segment fill (dumpn): large zero arrays, filled arrays with multi-byte encodings, odd sizes.
new gZero[100000];
new gZero2[4097];
new gOne[5000] = { 1, ... };
new gNeg[3001] = { -1, ... };
new gBig[2049] = { 0x12345678, ... };
new gMix[7] = { 0, 1, -1, 0x7FFFFFFF, 0x80000000, 64, -65 };
new gTwo[3][70000];

main()
{
    return gZero[99999] + gZero2[4096] + gOne[4999] + gNeg[3000] + gBig[2048] + gMix[6] + gTwo[2][69999];
}
