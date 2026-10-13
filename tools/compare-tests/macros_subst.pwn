// Macro substitution text: missing parameter (warning 236), "%0" inside a string, long expansions,
// expansions that are matched again by other macros, several macros on one line.
#include <console>

#define STR(%0) "value %0 stays", %0
#define LONG(%0) ((%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0)+(%0))
#define TWICE(%0) LONG(%0)+LONG(%0)
#define CHAIN1 CHAIN2+1
#define CHAIN2 CHAIN3+2
#define CHAIN3 3
#define PAIR(%0,%1) (%0)*(%1)

main()
{
    new a = 2, b = 3;
    printf(STR(7));
    printf("%d %d %d %d", TWICE(a), CHAIN1, PAIR(a, b) + PAIR(b, PAIR(a, a)), LONG(b) - LONG(a));
}
