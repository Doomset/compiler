// warning 236: a substitution names a parameter the pattern does not have
#define MISSING(%0) ((%0)+(%3))
main()
{
    new a = 1;
    return MISSING(a);
}
