// stripcomment(): comments, strings that look like comments, raw and packed strings, directives with raw text.
#include <console>

/* plain comment */ new gA = 1; /* another
   spanning
   lines */ new gB = 2;
/** documentation comment
 * with stars
 */
forward DocFunc();
/// single line documentation
public DocFunc() { return 3; }

/* nested /* comment (warning 216) */
new gC[] = "text // not a comment /* nor this */";
new gD[] = "quote \" inside";
new gE[] = \"raw \ string // still a string";
new gF[] = !"packed /* string */";
new gG = '/';
new gH = '\'';
new gI = '"';
new gJ[] = "multi\
line string";
#pragma warning disable 203
#warning "raw text // with a comment marker /* here */"
#define COMMENTED(%0) (%0) // trailing comment
#define TWO 2 /* block comment after a define */

main()
{
    new x = gA + gB + COMMENTED(gG) + TWO; // comment with "quotes" and 'q'
    /* comment */ x += gH /* inline */ + gI;
    printf("%d %s %s %s %s %s", x, gC, gD, gE, gF, gJ); //// many slashes
    return DocFunc();
}
