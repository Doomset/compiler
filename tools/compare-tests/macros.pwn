// Macro table: define, use, undef, redefine (warning 201), parameters, many macros on one first letter.
#include <console>

#define AA 1
#define AB 2
#define AC(%0) ((%0)*3)
#define AD%0Z (%0+4)
#define A_LONG_NAME_THAT_IS_LONG 5
#define AE AA+AB
#undef AB
#define AB 20
#define AB 21
#define AF(%0,%1) (%0-%1)
#undef AF

#define AF(%0,%1) (%1-%0)
#define x_hook:%0(%1) public x_on%0(%1)
#define BB<%0,%1> (%0*%1)

#if defined AA
  #define HAS_AA 1
#else
  #define HAS_AA 0
#endif
#if defined AB && !defined NOT_A_MACRO
  #define HAS_AB 1
#else
  #define HAS_AB 0
#endif

#define M1 1
#define M2 M1+1
#define M3 M2+1
#undef M1
#define M1 10

x_hook:Test(a)
{
    return a + AA;
}

main()
{
    new v = AA + AB + AC(2) + AD 5 Z + A_LONG_NAME_THAT_IS_LONG + AE + AF(1, 9) + HAS_AA + HAS_AB + M3;
    new w = BB<3,4>;
    printf("%d %d", v, w);
    x_onTest(1);
}
