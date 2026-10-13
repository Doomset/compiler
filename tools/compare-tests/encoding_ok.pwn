// Source encodings without a byte order mark: UTF-8 and CP1251 strings (each file is read in both passes).
#include "enc/cp1251.inc"
#include "enc/u8plain.inc"
main() { return CP() + U8P(); }
