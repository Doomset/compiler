/*  Pawn compiler - the state the first pass hands to the second pass
 *
 *  The second pass compiles against what the first pass left behind: the global symbol table
 *  (usage flags, tags, prototypes, array sizes, data addresses), the tag, library, automaton and
 *  state tables, the heap list and the settings a #pragma changes. pass_state_save() writes that
 *  state where the second pass would start; pass_state_load() puts it back instead of running
 *  the first pass. A build can then start the second pass of an edited source at once, from the
 *  state of the previous build, while a normal compile runs its first pass next to it.
 *
 *  The speculative output is only valid if the fresh first pass leaves the same state. Besides
 *  the state, pass_state_save() writes "<path>.key": the state without the fields the second
 *  pass sets again before anything reads them (declaration line numbers, code addresses and
 *  function addresses, stack sizes, referrer lists). Equal keys make equal second passes; the
 *  only things that may then still differ are the line numbers in warnings and the "estimated
 *  max. usage" line, so a caller trusts a speculative compile only when it has no messages.
 *
 *  Calls to functions go through symbolic labels ("call .name", #emit with a function name),
 *  which the assembler resolves, and __addressof() needs the function defined earlier in the
 *  same pass: the first pass's function addresses never reach the code of the second pass.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sc.h"

#define PS_MAGIC    "PAWNPASS2"
#define PS_END      0x50454e44L   /* "PEND" */

/* ----- writing ---------------------------------------------------------------------------- */

typedef struct {
  FILE *full;   /* everything pass_state_load() needs */
  FILE *key;    /* what decides the output of the second pass */
} pswriter;

static void put(pswriter *w,int inkey,const void *p,size_t n)
{
  fwrite(p,1,n,w->full);
  if (inkey)
    fwrite(p,1,n,w->key);
}

static void put_int(pswriter *w,int inkey,long v)
{
  int32_t x=(int32_t)v;
  put(w,inkey,&x,sizeof x);
}

static void put_cell(pswriter *w,int inkey,cell v)
{
  put(w,inkey,&v,sizeof v);
}

static void put_str(pswriter *w,int inkey,const char *s)
{
  long n=(s==NULL) ? -1 : (long)strlen(s);
  put_int(w,inkey,n);
  if (n>0)
    put(w,inkey,s,(size_t)n);
}

static void put_consttable(pswriter *w,int inkey,const constvalue_root *table)
{
  const constvalue *cv;
  long count=0;
  if (table!=NULL)
    for (cv=table->first; cv!=NULL; cv=cv->next)
      count++;
  put_int(w,inkey,(table==NULL) ? -1 : count);
  if (table!=NULL)
    for (cv=table->first; cv!=NULL; cv=cv->next) {
      put_str(w,inkey,cv->name);
      put_cell(w,inkey,cv->value);
      put_int(w,inkey,cv->index);
    } /* for */
}

/* the settings a #pragma or the command line may change, and the first pass does not reset */
#define PS_SCALARS(X) \
  X(sc_rationaltag) X(rational_digits) X(pc_stksize) X(pc_amxlimit) X(pc_amxram) \
  X(pc_compat) X(pc_optimize) X(sc_debug) X(sc_compress) X(sc_dataalign) X(pc_recursion) \
  X(sc_asmfile) X(sc_listing) X(sc_packstr) X(sc_needsemicolon) X(sc_tabsize) X(sc_ctrlchar) \
  X(pc_addlibtable) X(sc_alignnext) X(pc_naked) X(sc_allowtags)

static void put_arglist(pswriter *w,const arginfo *arglist)
{
  const arginfo *arg;
  long count=0;
  int i;
  for (arg=arglist; arg->ident!=0; arg++)
    count++;
  put_int(w,TRUE,count);
  for (arg=arglist; arg->ident!=0; arg++) {
    put_str(w,TRUE,arg->name);
    put_int(w,TRUE,arg->ident);
    put_int(w,TRUE,arg->usage);
    put_int(w,TRUE,arg->numtags);
    for (i=0; i<arg->numtags; i++)
      put_int(w,TRUE,arg->tags[i]);
    put_int(w,TRUE,arg->numdim);
    for (i=0; i<sDIMEN_MAX; i++) {
      /* the compiler leaves the slots past numdim uninitialized */
      put_int(w,TRUE,(i<arg->numdim) ? arg->dim[i] : 0);
      put_int(w,TRUE,(i<arg->numdim) ? arg->idxtag[i] : 0);
    } /* for */
    put_int(w,TRUE,arg->hasdefault);
    put_int(w,TRUE,arg->defvalue_tag);
    if (arg->ident==iREFARRAY && arg->hasdefault) {
      put_int(w,TRUE,arg->defvalue.array.size);
      put_int(w,TRUE,arg->defvalue.array.arraysize);
      put_cell(w,TRUE,arg->defvalue.array.addr);
      put(w,TRUE,arg->defvalue.array.data,(size_t)arg->defvalue.array.size*sizeof(cell));
    } else if (arg->ident==iVARIABLE && (arg->hasdefault & (uSIZEOF | uTAGOF))!=0) {
      put_str(w,TRUE,arg->defvalue.size.symname);
      put_int(w,TRUE,arg->defvalue.size.level);
    } else {
      put_cell(w,TRUE,arg->hasdefault ? arg->defvalue.val : 0);
    } /* if */
  } /* for */
}

static int is_saved(const symbol *sym)
{
  return (sym->flags & flagPREDEF)==0;
}

/* predefined constants whose value is not the first pass's business: __line changes on every
 * line, __timestamp is the time of this compile (it differs between any two builds) */
static int is_volatile_predef(const symbol *sym)
{
  return sym==line_sym || strcmp(sym->name,"__timestamp")==0;
}

static void finish_file(FILE *f,const char *tmp,const char *final,int *ok)
{
  if (fclose(f)!=0)
    *ok=FALSE;
  if (*ok) {
    remove(final);
    if (rename(tmp,final)!=0)
      *ok=FALSE;
  } /* if */
  if (!*ok)
    remove(tmp);
}

SC_FUNC int pass_state_save(const char *path)
{
  char tmp[_MAX_PATH+16],keytmp[_MAX_PATH+16],keyname[_MAX_PATH+16];
  pswriter w;
  symbol *sym;
  long count,index,i;
  valuepair *hp;
  int ok=TRUE;

  sprintf(tmp,"%s.tmp",path);
  sprintf(keyname,"%s.key",path);
  sprintf(keytmp,"%s.key.tmp",path);
  w.full=fopen(tmp,"wb");
  w.key=fopen(keytmp,"wb");
  if (w.full==NULL || w.key==NULL) {
    if (w.full!=NULL)
      fclose(w.full);
    if (w.key!=NULL)
      fclose(w.key);
    return FALSE;
  } /* if */

  put(&w,TRUE,PS_MAGIC,sizeof PS_MAGIC);
  put_int(&w,TRUE,(long)sizeof(cell));
  put_int(&w,TRUE,sNAMEMAX);

  #define PS_PUT_SCALAR(v) put_cell(&w,TRUE,(cell)(v));
  PS_SCALARS(PS_PUT_SCALAR)
  #undef PS_PUT_SCALAR

  put_consttable(&w,TRUE,&tagname_tab);
  put_consttable(&w,TRUE,&libname_tab);
  index=-1;
  if (curlibrary!=NULL) {
    constvalue *cv;
    for (cv=libname_tab.first, i=0; cv!=NULL && cv!=curlibrary; cv=cv->next, i++)
      /* nothing */;
    index=(cv!=NULL) ? i : -1;
  } /* if */
  put_int(&w,TRUE,index);
  put_consttable(&w,TRUE,&sc_automaton_tab);
  put_consttable(&w,TRUE,&sc_state_tab);
  {
    /* the state combinations (scstate.c), in table order */
    const int *states;
    int numstates,fsa,listid;
    count=0;
    while (state_list_get((int)count,&states,&numstates,&fsa,&listid))
      count++;
    put_int(&w,TRUE,count);
    for (i=0; i<count; i++) {
      long k;
      state_list_get((int)i,&states,&numstates,&fsa,&listid);
      put_int(&w,TRUE,numstates);
      put_int(&w,TRUE,fsa);
      put_int(&w,TRUE,listid);
      for (k=0; k<numstates; k++)
        put_int(&w,TRUE,states[k]);
    } /* for */
  }

  count=0;
  for (hp=heaplist_first(); hp!=NULL; hp=hp->next)
    count++;
  put_int(&w,TRUE,count);
  for (hp=heaplist_first(); hp!=NULL; hp=hp->next) {
    put_int(&w,TRUE,hp->first);
    put_int(&w,TRUE,hp->second);
  } /* for */

  /* predefined symbols: the constants made at startup and the string constants (__file, __date,
   * __time) every pass makes again; the second pass stores a string only if the first pass saw
   * it used, so the usage flags count */
  count=0;
  for (sym=glbtab.next; sym!=NULL; sym=sym->next)
    if (!is_saved(sym))
      count++;
  put_int(&w,TRUE,count);
  for (sym=glbtab.next; sym!=NULL; sym=sym->next) {
    if (is_saved(sym))
      continue;
    put_str(&w,TRUE,sym->name);
    put_int(&w,TRUE,sym->ident);
    put_int(&w,TRUE,sym->vclass);
    put_int(&w,TRUE,sym->tag);
    put_int(&w,TRUE,(unsigned short)sym->usage);
    put_int(&w,TRUE,sym->flags);
    /* __line and __timestamp are not the first pass's (is_volatile_predef), and a string constant gets its data address again
     * when the second pass makes it (add_builtin_string_constant) */
    put_cell(&w,!is_volatile_predef(sym) && sym->ident!=iARRAY,sym->addr);
  } /* for */

  /* number the symbols for the parent/child links and the referrer lists */
  count=0;
  for (sym=glbtab.next; sym!=NULL; sym=sym->next) {
    if (!is_saved(sym))
      continue;
    if (sym->vclass!=sGLOBAL)
      ok=FALSE;           /* pass_state_load() could not put it back where it was */
    sym->work=(int)++count;
  } /* for */
  put_int(&w,TRUE,count);
  for (sym=glbtab.next; sym!=NULL; sym=sym->next) {
    int isfunc;
    if (!is_saved(sym))
      continue;
    isfunc=(sym->ident==iFUNCTN);
    put_str(&w,TRUE,sym->name);
    put_int(&w,TRUE,sym->ident);
    put_int(&w,TRUE,sym->vclass);
    put_int(&w,TRUE,(unsigned short)sym->usage);
    put_int(&w,TRUE,sym->flags);
    put_int(&w,TRUE,sym->compound);
    put_int(&w,TRUE,sym->tag);
    put_int(&w,TRUE,sym->fnumber);
    put_int(&w,TRUE,sym->assignlevel);
    /* Functions and global variables get their address again in the second pass, and nothing
     * reads it earlier: a call goes through a label, and a variable cannot be used before its
     * declaration (error 017, also in #emit and sizeof). A string literal added to a function
     * moves the data address of every global declared below it, so these must stay out of the
     * key. The return array of a function (iREFARRAY) keeps its address in the key. */
    put_cell(&w,sym->ident==iREFARRAY,sym->addr);
    put_cell(&w,FALSE,sym->codeaddr);
    put_int(&w,FALSE,sym->lnumber);
    put_int(&w,TRUE,(sym->parent!=NULL && is_saved(sym->parent)) ? sym->parent->work : 0);
    put_int(&w,TRUE,(sym->child!=NULL && is_saved(sym->child)) ? sym->child->work : 0);
    if (isfunc) {
      put_int(&w,FALSE,(long)sym->x.stacksize);
      assert(sym->dim.arglist!=NULL);
      put_arglist(&w,sym->dim.arglist);
    } else {
      put_int(&w,TRUE,sym->x.tags.index);
      put_int(&w,TRUE,sym->x.tags.field);
      put_int(&w,TRUE,sym->x.tags.unique);
      put_cell(&w,TRUE,sym->dim.array.length);
      put_int(&w,TRUE,sym->dim.array.level);
    } /* if */
    put_consttable(&w,TRUE,sym->states);
    /* referrers: only for the stack estimate at the end, rebuilt by the second pass anyway */
    count=0;
    for (i=0; i<sym->numrefers; i++)
      if (sym->refer[i]!=NULL && is_saved(sym->refer[i]) && sym->refer[i]->work>0)
        count++;
    put_int(&w,FALSE,count);
    for (i=0; i<sym->numrefers; i++)
      if (sym->refer[i]!=NULL && is_saved(sym->refer[i]) && sym->refer[i]->work>0)
        put_int(&w,FALSE,sym->refer[i]->work);
  } /* for */
  for (sym=glbtab.next; sym!=NULL; sym=sym->next)
    sym->work=0;
  put_int(&w,TRUE,PS_END);

  if (ferror(w.full) || ferror(w.key))
    ok=FALSE;
  {
    int keyok=ok;
    finish_file(w.full,tmp,path,&ok);
    finish_file(w.key,keytmp,keyname,&keyok);
    ok=ok && keyok;
  }
  return ok;
}

/* ----- reading ---------------------------------------------------------------------------- */

typedef struct {
  FILE *f;
  int bad;
} psreader;

static int ps_debug=-1;
#define PSDBG(msg) do { if (ps_debug<0) ps_debug=(getenv("PAWNCC_PASS_DEBUG")!=NULL); if (ps_debug) { fprintf(stderr,"[pass] %s\n",msg); fflush(stderr); } } while (0)

static void get(psreader *r,void *p,size_t n)
{
  if (r->bad || fread(p,1,n,r->f)!=n) {
    r->bad=TRUE;
    memset(p,0,n);
  } /* if */
}

static long get_int(psreader *r)
{
  int32_t x;
  get(r,&x,sizeof x);
  return x;
}

static cell get_cell(psreader *r)
{
  cell x;
  get(r,&x,sizeof x);
  return x;
}

/* reads a string into a fresh malloc'ed block (NULL for a NULL string) */
static char *get_str(psreader *r)
{
  long n=get_int(r);
  char *s;
  if (n<0 || r->bad)
    return NULL;
  if (n>0x100000L) {
    r->bad=TRUE;
    return NULL;
  } /* if */
  if ((s=(char*)malloc((size_t)n+1))==NULL)
    error(103);           /* insufficient memory */
  get(r,s,(size_t)n);
  s[n]='\0';
  return s;
}

static void get_name(psreader *r,char *name)
{
  char *s=get_str(r);
  if (s==NULL || strlen(s)>sNAMEMAX) {
    r->bad=TRUE;
    name[0]='\0';
  } else {
    strcpy(name,s);
  } /* if */
  free(s);
}

/* fills (an empty) table; returns FALSE for a NULL table */
static int get_consttable(psreader *r,constvalue_root *table)
{
  long count=get_int(r),i;
  if (count<0)
    return FALSE;
  for (i=0; i<count && !r->bad; i++) {
    char name[sNAMEMAX+1];
    cell value;
    int index;
    get_name(r,name);
    value=get_cell(r);
    index=(int)get_int(r);
    append_constval(table,name,value,index);
  } /* for */
  return TRUE;
}

static constvalue_root *get_statelist(psreader *r)
{
  constvalue_root *root;
  if ((root=(constvalue_root*)malloc(sizeof(constvalue_root)))==NULL)
    error(103);
  root->first=root->last=NULL;
  if (!get_consttable(r,root)) {
    free(root);
    return NULL;
  } /* if */
  return root;
}

static arginfo *get_arglist(psreader *r)
{
  long count=get_int(r),n,i;
  arginfo *list;
  if (count<0 || count>10000 || r->bad) {
    r->bad=TRUE;
    count=0;
  } /* if */
  if ((list=(arginfo*)calloc((size_t)count+1,sizeof(arginfo)))==NULL)
    error(103);
  for (n=0; n<count && !r->bad; n++) {
    arginfo *arg=&list[n];
    get_name(r,arg->name);
    arg->ident=(char)get_int(r);
    arg->usage=(char)get_int(r);
    arg->numtags=(int)get_int(r);
    if (arg->numtags<0 || arg->numtags>1000) {
      r->bad=TRUE;
      arg->numtags=0;
    } /* if */
    if ((arg->tags=(int*)malloc(((size_t)arg->numtags+1)*sizeof(int)))==NULL)
      error(103);
    for (i=0; i<arg->numtags; i++)
      arg->tags[i]=(int)get_int(r);
    arg->numdim=(int)get_int(r);
    for (i=0; i<sDIMEN_MAX; i++) {
      arg->dim[i]=(int)get_int(r);
      arg->idxtag[i]=(int)get_int(r);
    } /* for */
    arg->hasdefault=(unsigned char)get_int(r);
    arg->defvalue_tag=(int)get_int(r);
    if (arg->ident==iREFARRAY && arg->hasdefault) {
      arg->defvalue.array.size=(int)get_int(r);
      arg->defvalue.array.arraysize=(int)get_int(r);
      arg->defvalue.array.addr=get_cell(r);
      if (arg->defvalue.array.size<0 || arg->defvalue.array.size>0x1000000) {
        r->bad=TRUE;
        arg->defvalue.array.size=0;
      } /* if */
      if ((arg->defvalue.array.data=(cell*)malloc(((size_t)arg->defvalue.array.size+1)*sizeof(cell)))==NULL)
        error(103);
      get(r,arg->defvalue.array.data,(size_t)arg->defvalue.array.size*sizeof(cell));
    } else if (arg->ident==iVARIABLE && (arg->hasdefault & (uSIZEOF | uTAGOF))!=0) {
      arg->defvalue.size.symname=get_str(r);
      arg->defvalue.size.level=(short)get_int(r);
      if (arg->defvalue.size.symname==NULL)
        r->bad=TRUE;
    } else {
      arg->defvalue.val=get_cell(r);
    } /* if */
  } /* for */
  list[count].ident=0;    /* terminator */
  return list;
}

typedef struct {
  char name[sNAMEMAX+1];
  int ident,vclass,flags,compound,tag,fnumber,assignlevel,lnumber,parent,child,level,numrefers;
  unsigned short usage;
  cell addr,codeaddr,length;
  long stacksize;
  int tagindex,tagfield,tagunique;
  arginfo *arglist;
  constvalue_root *states;
  int *refers;
  symbol *sym;
} psrecord;

SC_FUNC int pass_state_load(const char *path)
{
  psreader r;
  char magic[sizeof PS_MAGIC];
  psrecord *rec=NULL;
  long count,i,j;

  r.bad=FALSE;
  if ((r.f=fopen(path,"rb"))==NULL)
    return FALSE;
  get(&r,magic,sizeof magic);
  if (memcmp(magic,PS_MAGIC,sizeof magic)!=0 || get_int(&r)!=(long)sizeof(cell) || get_int(&r)!=sNAMEMAX) {
    fclose(r.f);
    return FALSE;
  } /* if */

  #define PS_GET_SCALAR(v) v=(int)get_cell(&r);
  #define PS_GET_SCALAR_CELL(v) v=get_cell(&r);
  PS_GET_SCALAR(sc_rationaltag) PS_GET_SCALAR(rational_digits) PS_GET_SCALAR_CELL(pc_stksize)
  PS_GET_SCALAR_CELL(pc_amxlimit) PS_GET_SCALAR_CELL(pc_amxram) PS_GET_SCALAR(pc_compat)
  PS_GET_SCALAR(pc_optimize) PS_GET_SCALAR(sc_debug) PS_GET_SCALAR(sc_compress)
  PS_GET_SCALAR(sc_dataalign) PS_GET_SCALAR(pc_recursion) PS_GET_SCALAR(sc_asmfile)
  PS_GET_SCALAR(sc_listing) PS_GET_SCALAR(sc_packstr) PS_GET_SCALAR(sc_needsemicolon)
  PS_GET_SCALAR(sc_tabsize)
  sc_ctrlchar=(char)get_cell(&r);
  PS_GET_SCALAR(pc_addlibtable) PS_GET_SCALAR(sc_alignnext) PS_GET_SCALAR(pc_naked)
  sc_allowtags=(short)get_cell(&r);
  #undef PS_GET_SCALAR
  #undef PS_GET_SCALAR_CELL

  PSDBG("scalars");
  delete_consttable(&tagname_tab);
  get_consttable(&r,&tagname_tab);
  PSDBG("tags");
  delete_consttable(&libname_tab);
  get_consttable(&r,&libname_tab);
  i=get_int(&r);
  curlibrary=NULL;
  if (i>=0) {
    constvalue *cv=libname_tab.first;
    while (cv!=NULL && i-->0)
      cv=cv->next;
    curlibrary=cv;
  } /* if */
  delete_consttable(&sc_automaton_tab);
  get_consttable(&r,&sc_automaton_tab);
  delete_consttable(&sc_state_tab);
  get_consttable(&r,&sc_state_tab);
  state_deletetable();
  count=get_int(&r);
  if (count<0 || count>0x100000)
    r.bad=TRUE;
  for (i=0; i<count && !r.bad; i++) {
    int numstates=(int)get_int(&r);
    int fsa=(int)get_int(&r);
    int listid=(int)get_int(&r);
    int *states;
    if (numstates<0 || numstates>0x10000) {
      r.bad=TRUE;
      break;
    } /* if */
    if ((states=(int*)malloc(((numstates>0) ? numstates : 1)*sizeof(int)))==NULL)
      error(103);
    for (j=0; j<numstates; j++)
      states[j]=(int)get_int(&r);
    state_list_append(states,numstates,fsa,listid);
    free(states);
  } /* for */

  PSDBG("tables");
  delete_heaplisttable();
  count=get_int(&r);
  for (i=0; i<count && !r.bad; i++) {
    long first=get_int(&r);
    long second=get_int(&r);
    push_heaplist(first,second);
  } /* for */

  count=get_int(&r);
  for (i=0; i<count && !r.bad; i++) {
    char name[sNAMEMAX+1];
    int ident,vclass,tag,flags;
    unsigned short usage;
    cell addr;
    symbol *sym;
    get_name(&r,name);
    ident=(int)get_int(&r);
    vclass=(int)get_int(&r);
    tag=(int)get_int(&r);
    usage=(unsigned short)get_int(&r);
    flags=(int)get_int(&r);
    addr=get_cell(&r);
    if (r.bad)
      break;
    for (sym=glbtab.next; sym!=NULL; sym=sym->next)
      if ((sym->flags & flagPREDEF)!=0 && sym->ident==ident && strcmp(sym->name,name)==0)
        break;
    if (sym==NULL) {
      /* a string constant (__file, __date, __time) the first pass made: the second pass finds it
       * and stores its text if the first pass saw it used */
      if (vclass!=sGLOBAL) {
        r.bad=TRUE;
        break;
      } /* if */
      sym=addsym(name,addr,ident,vclass,tag,usage);
    } /* if */
    if (!is_volatile_predef(sym))
      sym->addr=addr;     /* __timestamp stays the time of this compile */
    sym->usage=(short)usage;
    sym->flags=(char)flags;
  } /* for */

  PSDBG("predefined");
  count=get_int(&r);
  if (count<0 || count>0x1000000)
    r.bad=TRUE;
  if (!r.bad && (rec=(psrecord*)calloc((size_t)count+1,sizeof(psrecord)))==NULL)
    error(103);
  for (i=0; i<count && !r.bad; i++) {
    psrecord *p=&rec[i];
    get_name(&r,p->name);
    p->ident=(int)get_int(&r);
    p->vclass=(int)get_int(&r);
    p->usage=(unsigned short)get_int(&r);
    p->flags=(int)get_int(&r);
    p->compound=(int)get_int(&r);
    p->tag=(int)get_int(&r);
    p->fnumber=(int)get_int(&r);
    p->assignlevel=(int)get_int(&r);
    p->addr=get_cell(&r);
    p->codeaddr=get_cell(&r);
    p->lnumber=(int)get_int(&r);
    p->parent=(int)get_int(&r);
    p->child=(int)get_int(&r);
    if (p->ident==iFUNCTN) {
      p->stacksize=get_int(&r);
      p->arglist=get_arglist(&r);
    } else {
      p->tagindex=(int)get_int(&r);
      p->tagfield=(int)get_int(&r);
      p->tagunique=(int)get_int(&r);
      p->length=get_cell(&r);
      p->level=(int)get_int(&r);
    } /* if */
    p->states=get_statelist(&r);
    p->numrefers=(int)get_int(&r);
    if (p->numrefers<0 || p->numrefers>count) {
      r.bad=TRUE;
      p->numrefers=0;
    } /* if */
    if ((p->refers=(int*)malloc(((size_t)p->numrefers+1)*sizeof(int)))==NULL)
      error(103);
    for (j=0; j<p->numrefers; j++)
      p->refers[j]=(int)get_int(&r);
  } /* for */
  PSDBG("records read");
  if (get_int(&r)!=PS_END)
    r.bad=TRUE;
  fclose(r.f);
  if (r.bad) {
    if (rec!=NULL) {
      for (i=0; i<count; i++)
        free(rec[i].refers);
      free(rec);
    } /* if */
    return FALSE;
  } /* if */

  /* The table is sorted, and a symbol goes in front of the symbols with an equal name:
   * inserting backwards gives the saved order, equal names included. */
  PSDBG("inserting");
  for (i=count-1; i>=0; i--) {
    psrecord *p=&rec[i];
    symbol *sym;
    if ((i%5000)==0) PSDBG(p->name);
    sym=addsym(p->name,p->addr,p->ident,p->vclass,p->tag,p->usage);
    sym->flags=(char)p->flags;
    sym->compound=p->compound;
    sym->fnumber=p->fnumber;
    sym->assignlevel=p->assignlevel;
    sym->codeaddr=p->codeaddr;
    sym->lnumber=p->lnumber;
    if (p->ident==iFUNCTN) {
      sym->x.stacksize=p->stacksize;
      sym->dim.arglist=p->arglist;
    } else {
      sym->x.tags.index=p->tagindex;
      sym->x.tags.field=p->tagfield;
      sym->x.tags.unique=p->tagunique;
      sym->dim.array.length=p->length;
      sym->dim.array.level=(short)p->level;
    } /* if */
    sym->states=p->states;
    p->sym=sym;
  } /* for */
  PSDBG("linking");
  for (i=0; i<count; i++) {
    psrecord *p=&rec[i];
    symbol *sym=p->sym;
    sym->parent=(p->parent>0 && p->parent<=count) ? rec[p->parent-1].sym : NULL;
    sym->child=(p->child>0 && p->child<=count) ? rec[p->child-1].sym : NULL;
    free(sym->refer);
    if ((sym->refer=(symbol**)malloc(((size_t)p->numrefers+1)*sizeof(symbol*)))==NULL)
      error(103);
    for (j=0; j<p->numrefers; j++)
      sym->refer[j]=(p->refers[j]>0 && p->refers[j]<=count) ? rec[p->refers[j]-1].sym : NULL;
    if (p->numrefers==0)
      sym->refer[0]=NULL;
    sym->numrefers=(p->numrefers>0) ? p->numrefers : 1;
    free(p->refers);
  } /* for */
  free(rec);
  PSDBG("loaded");
  return TRUE;
}
