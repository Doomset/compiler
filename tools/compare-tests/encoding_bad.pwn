// A UTF-8 byte order mark on malformed UTF-8: error 77, also on the second include.
#include "enc/u8bad.inc"
#tryinclude "enc/u8bad.inc"
main() { return BAD(); }
