/*  Pawn compiler - checkpoints of a pass between two top-level declarations
 *
 *  A pass reads the whole source from the top. A checkpoint is the complete state of the compiler
 *  at a top-level declaration boundary (between two global declarations or functions): the symbol
 *  table with everything the pass has changed so far, the macros, the include levels with their
 *  positions, the lexer and preprocessor state, the counters (code and data addresses, labels)
 *  and the settings. A process that loads a checkpoint continues the pass from there and writes
 *  exactly what the uninterrupted pass would have written from there on.
 *
 *  So a pass can run in pieces ("segments"), each in a process of its own, all at the same time:
 *  segment k starts from checkpoint k (saved by an earlier compile) and stops at boundary k+1,
 *  where it saves its end state, its assembler text and its debug strings. The pieces belong
 *  together if the end state of every segment is byte for byte the start state of the next one;
 *  the caller checks that, and an assembler run (PAWNCC_ASM_FROM) builds the AMX from the pieces.
 *
 *  The file is canonical: equal states give equal files (no pointers, no padding, no slack past
 *  the end of a string), so the chain check is a plain file compare.
 *
 *  Environment:
 *    PAWNCC_CKPT_SAVE=<dir>     save checkpoints into <dir> ("<pass>-<ordinal>.ck", listed in
 *                               <dir>/index.txt)
 *    PAWNCC_CKPT_EVERY=<ms>     ... one per <ms> of processor time of the pass (default 150)
 *    PAWNCC_SEG_LOAD=<file>     start from a checkpoint instead of the top of the source
 *    PAWNCC_SEG_STOP=<ordinal>  stop at that boundary (without it: run to the end of the pass)
 *    PAWNCC_SEG_OUT=<prefix>    the segment writes <prefix>.end.ck, <prefix>.asm, <prefix>.dbg
 *                               and <prefix>.done ("errors warnings ordinal")
 *    PAWNCC_ASM_FROM=<list>     assemble: the first line names the final state, every further
 *                               line an .asm or .dbg piece, in order
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined _WIN32
  #include <windows.h>
#endif
#include "sc.h"

#define CK_MAGIC    "PAWNCKPT3"
#define CK_END      0x434b454eL   /* "CKEN" */

long ckpt_read_bytes=0;

struct s_ckio {
  FILE *f;
  int wr;
  int bad;
};

SC_FUNC void ck_bytes(ckio *io,void *p,size_t n)
{
  if (io->wr) {
    if (n>0 && fwrite(p,1,n,io->f)!=n)
      io->bad=TRUE;
  } else if (io->bad || (n>0 && fread(p,1,n,io->f)!=n)) {
    io->bad=TRUE;
    memset(p,0,n);
  } /* if */
}

SC_FUNC int ck_writing(ckio *io)
{
  return io->wr;
}

SC_FUNC void ck_fail(ckio *io)
{
  io->bad=TRUE;
}

/* writes n, or reads and returns the count (0 for a bad one) */
SC_FUNC int ck_count(ckio *io,int n)
{
  int32_t v=(int32_t)n;
  ck_bytes(io,&v,sizeof v);
  if (v<0 || v>0x4000000) {
    io->bad=TRUE;
    v=0;
  } /* if */
  return (int)v;
}

/* a string (NULL allowed); reading gives a fresh malloc'ed block */
SC_FUNC void ck_str(ckio *io,char **s)
{
  int32_t n;
  if (io->wr) {
    n=(*s==NULL) ? -1 : (int32_t)strlen(*s);
    ck_bytes(io,&n,sizeof n);
    if (n>0)
      ck_bytes(io,*s,(size_t)n);
  } else {
    *s=NULL;
    ck_bytes(io,&n,sizeof n);
    if (io->bad || n<0)
      return;
    if (n>0x1000000) {
      io->bad=TRUE;
      return;
    } /* if */
    if ((*s=(char*)malloc((size_t)n+1))==NULL)
      error(103);               /* insufficient memory */
    ck_bytes(io,*s,(size_t)n);
    (*s)[n]='\0';
  } /* if */
}

/* a fixed name buffer (sNAMEMAX+1), without what follows the terminator */
static void ck_name(ckio *io,char *name)
{
  int32_t n;
  if (io->wr) {
    n=(int32_t)strlen(name);
    ck_bytes(io,&n,sizeof n);
    ck_bytes(io,name,(size_t)n);
  } else {
    ck_bytes(io,&n,sizeof n);
    if (n<0 || n>sNAMEMAX) {
      io->bad=TRUE;
      n=0;
    } /* if */
    ck_bytes(io,name,(size_t)n);
    name[n]='\0';
  } /* if */
}

static int constval_index(constvalue_root *table,constvalue *item)
{
  constvalue *cv;
  int i;
  if (item==NULL)
    return -1;
  for (cv=table->first, i=0; cv!=NULL; cv=cv->next, i++)
    if (cv==item)
      return i;
  return -2;
}

static constvalue *constval_at(constvalue_root *table,int index)
{
  constvalue *cv;
  if (index<0)
    return NULL;
  for (cv=table->first; cv!=NULL && index>0; cv=cv->next)
    index--;
  return cv;
}

SC_FUNC void ck_libref(ckio *io,constvalue **lib)
{
  int32_t i=io->wr ? (int32_t)constval_index(&libname_tab,*lib) : 0;
  ck_bytes(io,&i,sizeof i);
  if (i==-2)
    io->bad=TRUE;
  if (!io->wr)
    *lib=constval_at(&libname_tab,i);
}

/* a constant table; *root may be NULL (a "has" flag goes first) when "may_null" */
static void ck_consttable(ckio *io,constvalue_root *root)
{
  constvalue *cv;
  int n=0,i;
  if (io->wr)
    for (cv=root->first; cv!=NULL; cv=cv->next)
      n++;
  n=ck_count(io,n);
  if (io->wr) {
    for (cv=root->first; cv!=NULL; cv=cv->next) {
      ck_name(io,cv->name);
      CKV(io,cv->value);
      CKV(io,cv->index);
    } /* for */
  } else {
    delete_consttable(root);
    for (i=0; i<n && !io->bad; i++) {
      char name[sNAMEMAX+1];
      cell value;
      int index;
      ck_name(io,name);
      CKV(io,value);
      CKV(io,index);
      append_constval(root,name,value,index);
    } /* for */
  } /* if */
}

static void ck_consttable_ptr(ckio *io,constvalue_root **root)
{
  int has=io->wr ? (*root!=NULL) : 0;
  CKV(io,has);
  if (!io->wr) {
    *root=NULL;
    if (has) {
      if ((*root=(constvalue_root*)calloc(1,sizeof(constvalue_root)))==NULL)
        error(103);
    } /* if */
  } /* if */
  if (has && *root!=NULL)
    ck_consttable(io,*root);
}

static void ck_arglist(ckio *io,arginfo **plist)
{
  arginfo *list=*plist,*arg;
  int n=0,i,k;
  if (io->wr)
    for (arg=list; arg->ident!=0; arg++)
      n++;
  n=ck_count(io,n);
  if (!io->wr) {
    if ((list=(arginfo*)calloc((size_t)n+1,sizeof(arginfo)))==NULL)
      error(103);
    *plist=list;
  } /* if */
  for (i=0; i<n && !io->bad; i++) {
    arg=&list[i];
    ck_name(io,arg->name);
    CKV(io,arg->ident);
    CKV(io,arg->usage);
    arg->numtags=ck_count(io,arg->numtags);
    if (!io->wr) {
      if ((arg->tags=(int*)malloc(((size_t)arg->numtags+1)*sizeof(int)))==NULL)
        error(103);
    } /* if */
    for (k=0; k<arg->numtags; k++)
      CKV(io,arg->tags[k]);
    CKV(io,arg->numdim);
    if (arg->numdim<0 || arg->numdim>sDIMEN_MAX) {
      io->bad=TRUE;
      break;
    } /* if */
    for (k=0; k<arg->numdim; k++) {
      CKV(io,arg->dim[k]);
      CKV(io,arg->idxtag[k]);
    } /* for */
    CKV(io,arg->hasdefault);
    CKV(io,arg->defvalue_tag);
    if (arg->ident==iREFARRAY && arg->hasdefault) {
      CKV(io,arg->defvalue.array.size);
      CKV(io,arg->defvalue.array.arraysize);
      CKV(io,arg->defvalue.array.addr);
      if (arg->defvalue.array.size<0 || arg->defvalue.array.size>0x1000000) {
        io->bad=TRUE;
        break;
      } /* if */
      if (!io->wr) {
        if ((arg->defvalue.array.data=(cell*)malloc(((size_t)arg->defvalue.array.size+1)*sizeof(cell)))==NULL)
          error(103);
      } /* if */
      ck_bytes(io,arg->defvalue.array.data,(size_t)arg->defvalue.array.size*sizeof(cell));
    } else if (arg->ident==iVARIABLE && (arg->hasdefault & (uSIZEOF | uTAGOF))!=0) {
      ck_str(io,&arg->defvalue.size.symname);
      CKV(io,arg->defvalue.size.level);
    } else {
      cell v=io->wr ? (arg->hasdefault ? arg->defvalue.val : 0) : 0;
      CKV(io,v);
      if (!io->wr)
        arg->defvalue.val=v;
    } /* if */
  } /* for */
}

/* ----- the symbol table ---------------------------------------------------------------------- */

typedef struct {
  symbol s;             /* the fields, the pointers not yet resolved */
  int parent,child;     /* 1-based numbers, 0 for none */
  int *refers;
  int numrefers;
  int htnext;           /* next symbol in the symbol cache chain */
} ckrec;

/* referrers in a canonical order close to the one the compiler builds (the order its callers come
 * in the code): by address, then by place in the table; the stack estimate walks them in this order
 * and its memo works best in source order */
static int cmp_refsym(const void *a,const void *b)
{
  const symbol *x=*(symbol*const*)a,*y=*(symbol*const*)b;
  if (x->addr!=y->addr)
    return (x->addr>y->addr) ? 1 : -1;
  return (x->work>y->work)-(x->work<y->work);
}

static int symref(symbol *sym)
{
  return (sym!=NULL) ? sym->work : 0;
}

static void ck_symbols(ckio *io,symbol **pcurfunc)
{
  symbol *sym;
  ckrec *rec=NULL;
  int count=0,i,k;

  if (io->wr) {
    for (sym=glbtab.next; sym!=NULL; sym=sym->next)
      sym->work=++count;
  } /* if */
  count=ck_count(io,count);
  if (!io->wr) {
    if ((rec=(ckrec*)calloc((size_t)count+1,sizeof(ckrec)))==NULL)
      error(103);
    sym=NULL;
  } else {
    sym=glbtab.next;
  } /* if */
  for (i=0; i<count && !io->bad; i++) {
    symbol *s=io->wr ? sym : &rec[i].s;
    int parent=io->wr ? symref(sym->parent) : 0;
    int child=io->wr ? symref(sym->child) : 0;
    int htnext=io->wr ? symref(sym->htnext) : 0;
    int nref=0;
    ck_name(io,s->name);
    CKV(io,s->addr);
    CKV(io,s->codeaddr);
    CKV(io,s->vclass);
    CKV(io,s->ident);
    CKV(io,s->usage);
    CKV(io,s->flags);
    CKV(io,s->compound);
    CKV(io,s->tag);
    CKV(io,s->assignlevel);
    CKV(io,s->fnumber);
    CKV(io,s->lnumber);
    CKV(io,s->lfile);
    CKV(io,s->memo_stack);
    CKV(io,s->memo_valid);
    CKV(io,parent);
    CKV(io,child);
    CKV(io,htnext);
    if (s->ident==iFUNCTN) {
      if ((s->usage & uNATIVE)!=0)
        ck_libref(io,&s->x.lib);
      else
        CKV(io,s->x.stacksize);
      if (io->wr) {
        assert(s->dim.arglist!=NULL);
      } /* if */
      ck_arglist(io,&s->dim.arglist);
    } else if (s->ident==iLABEL) {
      CKV(io,s->x.declared);
      CKV(io,s->dim.array.length);
      CKV(io,s->dim.array.level);
    } else {
      CKV(io,s->x.tags.index);
      CKV(io,s->x.tags.field);
      CKV(io,s->x.tags.unique);
      if (s->ident==iCONSTEXPR && (s->usage & uENUMROOT)==uENUMROOT) {
        ck_consttable_ptr(io,&s->dim.enumlist);
      } else {
        CKV(io,s->dim.array.length);
        CKV(io,s->dim.array.level);
      } /* if */
    } /* if */
    ck_consttable_ptr(io,&s->states);
    ck_str(io,&s->documentation);
    /* the referrer list: the symbols that use this one */
    if (io->wr) {
      for (k=0; k<sym->numrefers; k++)
        if (sym->refer[k]!=NULL && symref(sym->refer[k])>0)
          nref++;
    } /* if */
    nref=ck_count(io,nref);
    if (io->wr) {
      /* a set: the compiler fills holes of the array, a loaded one has none, so the order is
       * history and stays out of the file (the referrers only feed the stack estimate); a
       * referrer that is no longer in the table (a deleted operator: a dangling pointer the
       * compiler keeps) is left out */
      symbol **rs=(symbol**)malloc(((size_t)nref+1)*sizeof(symbol*));
      int m=0;
      if (rs==NULL)
        error(103);
      for (k=0; k<sym->numrefers; k++)
        if (sym->refer[k]!=NULL && symref(sym->refer[k])>0)
          rs[m++]=sym->refer[k];
      qsort(rs,(size_t)m,sizeof(symbol*),cmp_refsym);
      for (k=0; k<m; k++) {
        int r=symref(rs[k]);
        CKV(io,r);
      } /* for */
      free(rs);
      sym=sym->next;
    } else {
      rec[i].parent=parent;
      rec[i].child=child;
      rec[i].htnext=htnext;
      rec[i].numrefers=nref;
      if ((rec[i].refers=(int*)malloc(((size_t)nref+1)*sizeof(int)))==NULL)
        error(103);
      for (k=0; k<nref; k++)
        CKV(io,rec[i].refers[k]);
    } /* if */
  } /* for */
  i=io->wr ? symref(*pcurfunc) : 0;
  CKV(io,i);

  if (io->wr) {
    for (sym=glbtab.next; sym!=NULL; sym=sym->next)
      sym->work=0;
    return;
  } /* if */
  if (io->bad) {
    /* the process gives up anyway */
    free(rec);
    return;
  } /* if */

  /* the list goes back in the saved order (it is not quite sorted: rename_symbol() keeps the place
   * of a renamed operator), and so do the chains of the symbol cache, which decide which of two
   * symbols with one name a lookup finds */
  while (glbtab.next!=NULL)
    delete_symbol(&glbtab,glbtab.next);   /* the predefined ones too: they come from the file */
  line_sym=NULL;
  {
    symbol **made=(symbol**)calloc((size_t)count+1,sizeof(symbol*));
    int *htnext=(int*)calloc((size_t)count+1,sizeof(int));
    if (made==NULL || htnext==NULL)
      error(103);
    for (k=0; k<count; k++) {
      symbol *n=(symbol*)malloc(sizeof(symbol));
      if (n==NULL)
        error(103);
      memcpy(n,&rec[k].s,sizeof(symbol));
      n->next=n->parent=n->child=n->htnext=NULL;
      n->refer=NULL;
      n->work=0;
      made[k]=n;
      htnext[k]=rec[k].htnext;
    } /* for */
    for (k=0; k<count; k++) {
      ckrec *p=&rec[k];
      symbol *n=made[k];
      int j;
      n->parent=(p->parent>0 && p->parent<=count) ? made[p->parent-1] : NULL;
      n->child=(p->child>0 && p->child<=count) ? made[p->child-1] : NULL;
      if ((n->refer=(symbol**)malloc(((size_t)p->numrefers+1)*sizeof(symbol*)))==NULL)
        error(103);
      for (j=0; j<p->numrefers; j++)
        n->refer[j]=(p->refers[j]>0 && p->refers[j]<=count) ? made[p->refers[j]-1] : NULL;
      if (p->numrefers==0)
        n->refer[0]=NULL;
      n->numrefers=(p->numrefers>0) ? p->numrefers : 1;
      free(p->refers);
      if ((n->flags & flagPREDEF)!=0 && strcmp(n->name,"__line")==0)
        line_sym=n;
    } /* for */
    ckpt_glbtab_link(made,count,htnext);
    *pcurfunc=(i>0 && i<=count) ? made[i-1] : NULL;
    free(made);
    free(htnext);
  }
  free(rec);
}

/* ----- the whole state ----------------------------------------------------------------------- */

#define CK_SCALARS(X) \
  X(litidx) X(sc_labnum) X(staging) X(declared) X(glb_declared) X(code_idx) X(ntv_funcid) \
  X(errnum) X(warnnum) X(sc_debug) X(sc_packstr) X(sc_asmfile) X(sc_listing) X(sc_compress) \
  X(sc_needsemicolon) X(sc_dataalign) X(sc_alignnext) X(pc_docexpr) X(curseg) X(pc_stksize) \
  X(pc_amxlimit) X(pc_amxram) X(freading) X(fline) X(fnumber) X(fcurrent) X(sc_intest) \
  X(pc_sideeffect) X(pc_ovlassignment) X(stmtindent) X(indent_nowarn) X(sc_tabsize) \
  X(sc_allowtags) X(sc_status) X(sc_rationaltag) X(rational_digits) X(sc_allowproccall) \
  X(sc_is_utf8) X(sc_curstates) X(pc_optimize) X(pc_memflags) X(pc_naked) X(pc_compat) \
  X(pc_recursion) X(pc_retexpr) X(pc_retheap) X(pc_nestlevel) X(pc_attributes) \
  X(pc_ispackedstr) X(pc_isrecording) X(pc_loopcond) X(pc_numloopvars) X(sc_ctrlchar) \
  X(sc_ctrlchar_org) X(pc_addlibtable) X(emit_flags) X(emit_stgbuf_idx)

#define CK_MARK(io,what) do { if ((io)->wr && getenv("PAWNCC_CKPT_DEBUG")!=NULL) fprintf(stderr,"[ckpt] %s ends at %ld\n",what,ftell((io)->f)); } while (0)

static long ck_ordinal=0;       /* number of the boundary (counts eligible boundaries of the pass) */

static void ck_state(ckio *io)
{
  char magic[sizeof CK_MAGIC];
  int32_t v;

  memcpy(magic,CK_MAGIC,sizeof magic);
  ck_bytes(io,magic,sizeof magic);
  if (memcmp(magic,CK_MAGIC,sizeof magic)!=0)
    io->bad=TRUE;
  v=(int32_t)sizeof(cell);
  CKV(io,v);
  if (v!=(int32_t)sizeof(cell))
    io->bad=TRUE;
  v=sNAMEMAX;
  CKV(io,v);
  if (v!=sNAMEMAX)
    io->bad=TRUE;
  CKV(io,ck_ordinal);

  #define CK_SCALAR(var) CKV(io,var);
  CK_SCALARS(CK_SCALAR)
  #undef CK_SCALAR
  ck_str(io,&pc_deprecate);
  ck_str(io,&pc_recstr);

  ck_consttable(io,&tagname_tab);
  ck_consttable(io,&libname_tab);
  ck_libref(io,&curlibrary);
  ck_consttable(io,&sc_automaton_tab);
  ck_consttable(io,&sc_state_tab);
  {
    /* the state combinations (scstate.c), in table order */
    const int *states;
    int numstates,fsa,listid,count=0,i,k;
    if (io->wr)
      while (state_list_get(count,&states,&numstates,&fsa,&listid))
        count++;
    count=ck_count(io,count);
    if (!io->wr)
      state_deletetable();
    for (i=0; i<count && !io->bad; i++) {
      int *buf=NULL;
      if (io->wr)
        state_list_get(i,&states,&numstates,&fsa,&listid);
      numstates=ck_count(io,numstates);
      CKV(io,fsa);
      CKV(io,listid);
      if ((buf=(int*)malloc(((size_t)numstates+1)*sizeof(int)))==NULL)
        error(103);
      if (io->wr)
        memcpy(buf,states,(size_t)numstates*sizeof(int));
      ck_bytes(io,buf,(size_t)numstates*sizeof(int));
      if (!io->wr)
        state_list_append(buf,numstates,fsa,listid);
      free(buf);
    } /* for */
  }
  CK_MARK(io,"tables");
  ck_symbols(io,&curfunc);
  CK_MARK(io,"symbols");
  ckpt_visit_sc1(io);
  CK_MARK(io,"sc1");
  ckpt_visit_sc2(io);
  CK_MARK(io,"sc2");
  ckpt_visit_sc3(io);
  ckpt_visit_sc4(io);
  ckpt_visit_sc5(io);
  CK_MARK(io,"sc3-5");
  ckpt_visit_sclist(io);
  CK_MARK(io,"sclist");
  v=CK_END;
  CKV(io,v);
  if (v!=CK_END)
    io->bad=TRUE;
}

/* PAWNCC_CKPT_TEXT: a readable copy "<path>.txt" next to every saved checkpoint, for finding what
 * differs between two of them */
static void ck_text(const char *path)
{
  char name[_MAX_PATH+16];
  FILE *f;
  symbol *sym;
  int i,n;
  sprintf(name,"%s.txt",path);
  if ((f=fopen(name,"wb"))==NULL)
    return;
  fprintf(f,"ordinal %ld litidx %d labnum %d declared %ld glb %ld code %ld ntv %d err %d warn %d fline %d fnumber %d fcurrent %d status %d curstates %d\n",
          ck_ordinal,litidx,sc_labnum,(long)declared,(long)glb_declared,(long)code_idx,ntv_funcid,errnum,warnnum,fline,fnumber,fcurrent,sc_status,sc_curstates);
  fprintf(f,"curfunc %s inpfname %s\n",(curfunc!=NULL) ? curfunc->name : "-",(inpfname!=NULL) ? inpfname : "-");
  for (sym=glbtab.next; sym!=NULL; sym=sym->next) {
    for (i=0,n=0; i<sym->numrefers; i++)
      if (sym->refer[i]!=NULL)
        n++;
    fprintf(f,"%s id%d vc%d u%04x f%d c%d t%d a%ld ca%ld ln%d fn%d al%d refs%d st%ld par%s ch%s doc%d\n",
            sym->name,sym->ident,sym->vclass,(unsigned short)sym->usage,sym->flags,sym->compound,sym->tag,
            (long)sym->addr,(long)sym->codeaddr,sym->lnumber,sym->fnumber,sym->assignlevel,n,
            (sym->ident==iFUNCTN && (sym->usage & uNATIVE)==0) ? sym->x.stacksize : 0L,
            (sym->parent!=NULL) ? sym->parent->name : "-",(sym->child!=NULL) ? sym->child->name : "-",
            sym->documentation!=NULL);
  } /* for */
  fclose(f);
}

static double ck_wall(void)
{
#if defined _WIN32
  LARGE_INTEGER f,c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart/(double)f.QuadPart;
#else
  return (double)clock()/CLOCKS_PER_SEC;
#endif
}

static int ck_dbg=-1;
static void ck_timing(const char *what,double t0)
{
  if (ck_dbg<0)
    ck_dbg=(getenv("PAWNCC_CKPT_DEBUG")!=NULL);
  if (ck_dbg)
    fprintf(stderr,"[ckpt] %s %.3f s\n",what,ck_wall()-t0);
}

static int ck_save(const char *path)
{
  double t0=ck_wall();
  char tmp[_MAX_PATH+16];
  ckio io;
  int ok;
  sprintf(tmp,"%s.tmp",path);
  io.wr=TRUE;
  io.bad=FALSE;
  if ((io.f=fopen(tmp,"wb"))==NULL)
    return FALSE;
  setvbuf(io.f,NULL,_IOFBF,1<<20);
  ck_state(&io);
  ok=!io.bad && !ferror(io.f);
  if (fclose(io.f)!=0)
    ok=FALSE;
  if (ok) {
    remove(path);
    ok=(rename(tmp,path)==0);
  } /* if */
  if (!ok)
    remove(tmp);
  if (ok && getenv("PAWNCC_CKPT_TEXT")!=NULL)
    ck_text(path);
  ck_timing("save",t0);
  return ok;
}

static int ck_load(const char *path)
{
  double t0=ck_wall();
  ckio io;
  io.wr=FALSE;
  io.bad=FALSE;
  if ((io.f=fopen(path,"rb"))==NULL)
    return FALSE;
  setvbuf(io.f,NULL,_IOFBF,1<<20);
  ck_state(&io);
  fclose(io.f);
  ck_timing("load",t0);
  return !io.bad;
}

/* ----- the files a stretch of the pass reads ------------------------------------------------- *
 * "F <name>" for a file it opened, "D <directory>" for the directory of a name it looked for and did
 * not find (a new file there could change what an #include finds). A caller compares sizes and
 * times of these to decide which segments an edit touched. */

static char **ck_files=NULL;
static int ck_nfiles=0,ck_capfiles=0;

static void ck_file_add(const char *kind,const char *name)
{
  char entry[_MAX_PATH+8];
  int i;
  if (strlen(name)>_MAX_PATH)
    return;
  sprintf(entry,"%s %s",kind,name);
  for (i=0; i<ck_nfiles; i++)
    if (strcmp(ck_files[i],entry)==0)
      return;
  if (ck_nfiles==ck_capfiles) {
    int cap=(ck_capfiles==0) ? 64 : 2*ck_capfiles;
    char **p=(char**)realloc(ck_files,(size_t)cap*sizeof(char*));
    if (p==NULL)
      return;
    ck_files=p;
    ck_capfiles=cap;
  } /* if */
  ck_files[ck_nfiles++]=duplicatestring(entry);
}

static void ck_srcopen(const char *name,int found)
{
  if (found) {
    ck_file_add("F",name);
  } else {
    char dir[_MAX_PATH+1];
    char *p;
    strcpy(dir,name);
    p=strrchr(dir,'\\');
    if (p==NULL || (strrchr(dir,'/')!=NULL && strrchr(dir,'/')>p))
      p=strrchr(dir,'/');
    if (p!=NULL)
      *p='\0';
    else
      strcpy(dir,".");
    ck_file_add("D",dir);
  } /* if */
}

/* the files of the include levels open now: a stretch that starts here goes on reading them */
static void ck_files_reset(void)
{
  int i;
  for (i=0; i<ck_nfiles; i++)
    free(ck_files[i]);
  ck_nfiles=0;
  for (i=0; i<ckpt_level_count(); i++) {
    int fnum,line;
    FILE *h;
    ckpt_level_get(i,&fnum,&line,&h);
    if (h!=NULL && pc_srcname(h)!=NULL)
      ck_file_add("F",pc_srcname(h));
  } /* for */
}

static void ck_files_write(const char *path)
{
  FILE *f;
  int i;
  if ((f=fopen(path,"wb"))==NULL)
    return;
  for (i=0; i<ck_nfiles; i++)
    fprintf(f,"%s\n",ck_files[i]);
  fclose(f);
}

/* ----- driving ------------------------------------------------------------------------------- */

static int ck_mode=-1;          /* -1 not read yet, 0 off, 1 on */
static const char *ck_savedir=NULL;
static double ck_every=0.150;
static double ck_last=0;
static long ck_stop=-1;
static const char *ck_out=NULL;
static int ck_dbgstart=0;
static int ck_skip_next=0;      /* the loaded state was saved at this very boundary: do not count it again */       /* debug strings that were there when the segment started */

static double ck_cputime(void)
{
#if defined _WIN32
  FILETIME c,e,k,u;
  if (GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)) {
    ULARGE_INTEGER a,b;
    a.LowPart=k.dwLowDateTime; a.HighPart=k.dwHighDateTime;
    b.LowPart=u.dwLowDateTime; b.HighPart=u.dwHighDateTime;
    return (double)(a.QuadPart+b.QuadPart)/1e7;
  } /* if */
#endif
  return (double)clock()/CLOCKS_PER_SEC;
}

SC_FUNC void ckpt_init(void)
{
  const char *s;
  if (ck_mode>=0)
    return;
  ck_savedir=getenv("PAWNCC_CKPT_SAVE");
  if (ck_savedir!=NULL && *ck_savedir=='\0')
    ck_savedir=NULL;
  if ((s=getenv("PAWNCC_CKPT_EVERY"))!=NULL && atoi(s)>0)
    ck_every=atoi(s)/1000.0;
  if ((s=getenv("PAWNCC_SEG_STOP"))!=NULL && *s!='\0')
    ck_stop=atol(s);
  ck_out=getenv("PAWNCC_SEG_OUT");
  if (ck_out!=NULL && *ck_out=='\0')
    ck_out=NULL;
  ck_mode=(ck_savedir!=NULL || ck_stop>=0 || ck_out!=NULL) ? 1 : 0;
  ck_last=ck_cputime();
  if (ck_mode>0)
    pc_srcopen_hook=ck_srcopen;
}

SC_FUNC int ckpt_active(void)
{
  if (ck_mode<0)
    ckpt_init();
  return ck_mode>0;
}

static void ck_write_outputs(void)
{
  double t0=ck_wall();
  char name[_MAX_PATH+16];
  FILE *f;
  int i,n;

  if (ck_out==NULL)
    return;
  /* the assembler text written since the start */
  sprintf(name,"%s.asm",ck_out);
  if ((f=fopen(name,"wb"))!=NULL) {
    long len=0;
    const char *buf=pc_asmbuffer(outf,&len);
    if (buf!=NULL && len>0)
      fwrite(buf,1,(size_t)len,f);
    fclose(f);
  } /* if */
  sprintf(name,"%s.dbg",ck_out);
  if ((f=fopen(name,"wb"))!=NULL) {
    setvbuf(f,NULL,_IOFBF,1<<20);
    n=dbgstring_count();
    for (i=ck_dbgstart; i<n; i++) {
      fputs(get_dbgstring(i),f);
      fputc('\n',f);
    } /* for */
    fclose(f);
  } /* if */
  ck_timing("outputs",t0);
}

static void ck_write_done(int ok)
{
  char name[_MAX_PATH+16];
  FILE *f;
  if (ck_out==NULL)
    return;
  sprintf(name,"%s.files",ck_out);
  ck_files_write(name);
  sprintf(name,"%s.done",ck_out);
  if ((f=fopen(name,"wb"))!=NULL) {
    fprintf(f,"%d %d %d %ld\n",ok,errnum,warnnum,ck_ordinal);
    fclose(f);
  } /* if */
}

static int ck_eligible(void)
{
  return freading && (sc_status==statFIRST || sc_status==statWRITE)
         && ckpt_lexer_idle() && ckpt_sc7_idle()
         && litidx==0 && loctab.next==NULL && pc_nestlevel==0 && errnum==0
         && pc_deprecate==NULL && pc_attributes==0 && !pc_isrecording;
}

/* a pass starts from the top of the source */
SC_FUNC void ckpt_pass_begin(void)
{
  ckpt_init();
  ck_ordinal=0;
  ck_last=ck_cputime();
  if (ck_mode>0)
    ck_files_reset();
}

/* a normal compile that saves checkpoints: the files of the last stretch of the pass */
SC_FUNC void ckpt_pass_finish(void)
{
  char name[_MAX_PATH+64];
  if (ck_mode<=0 || ck_savedir==NULL)
    return;
  sprintf(name,"%s/%d-end.pre.files",ck_savedir,(sc_status==statFIRST) ? 1 : 2);
  ck_files_write(name);
}

/* called by parse() before every top-level declaration */
SC_FUNC void ckpt_boundary(void)
{
  if (ck_skip_next) {
    ck_skip_next=0;
    return;
  } /* if */
  if (!ck_eligible())
    return;
  ck_ordinal++;
  if (ck_stop>=0 && ck_ordinal==ck_stop) {
    char name[_MAX_PATH+16];
    int ok;
    sprintf(name,"%s.end.ck",(ck_out!=NULL) ? ck_out : "segment");
    ok=ck_save(name);
    ck_write_outputs();
    ck_write_done(ok);
    fflush(stdout);
    fflush(stderr);
    exit(ok ? 0 : 3);
  } /* if */
  if (ck_savedir!=NULL) {
    double t=ck_cputime();
    if (t-ck_last>=ck_every) {
      char name[_MAX_PATH+64];
      FILE *f;
      int pass=(sc_status==statFIRST) ? 1 : 2;
      sprintf(name,"%s/%d-%ld.pre.files",ck_savedir,pass,ck_ordinal);
      ck_files_write(name);
      ck_files_reset();
      sprintf(name,"%s/%d-%ld.ck",ck_savedir,pass,ck_ordinal);
      if (ck_save(name)) {
        sprintf(name,"%s/index.txt",ck_savedir);
        if ((f=fopen(name,"ab"))!=NULL) {
          fprintf(f,"%d %ld %ld %.3f\n",pass,ck_ordinal,ckpt_read_bytes,t);
          fclose(f);
        } /* if */
      } /* if */
      ck_last=ck_cputime();     /* the save itself does not count */
    } /* if */
  } /* if */
}

/* ----- relocation ---------------------------------------------------------------------------- *
 *
 * An edit inside a function body of segment j moves what comes after it: code and data addresses,
 * label numbers, line numbers and file positions in the edited file; it may also change usage
 * flags and referrers. The old checkpoints after it are then off by that difference. With
 * PAWNCC_SEG_RELOC_A=<a> and PAWNCC_SEG_RELOC_B=<b> a segment applies the difference between
 * <a> (the old start of segment j+1) and <b> (the new end of segment j) to its own start:
 *   - counters (code_idx, glb_declared, labels): plus the difference;
 *   - a symbol field that segment j changed and that is unchanged since: the new value; one that
 *     was set after <a> (a function or variable defined later): shifted by the code or data
 *     difference, a line number by the line difference of its file;
 *   - usage flags: the bits segment j changed flip; referrers: the set difference;
 *   - include levels of a file whose position moved: line and position plus the difference.
 * This is a guess with a check: the segment's end state must still equal the next start.
 */

typedef struct {
  char *key;
  cell addr,codeaddr;
  int usage,lnumber,lfile;
  long stacksize;
  int nrefs;
  char **refs;          /* keys of the referrers, sorted */
  int nargs;
  cell *argaddr;        /* data addresses of default arrays of the arguments (0 for the others) */
  int nstates;
  cell *statev;         /* values of the state list (code addresses of the state implementations) */
} cksym;

typedef struct {
  cell code_idx,glb_declared;
  int labnum,errnum,warnnum;
  int msg[7];           /* sc5.c: the state of the error messages */
  int nheap;
  long *heap;           /* the heap list (sclist.c), as pairs */
  int nlev;
  int lev_f[sCOMP_STACK+64],lev_line[sCOMP_STACK+64];
  size_t lev_pos[sCOMP_STACK+64];
  int nsym;
  cksym *syms;
  int *ht;
  int htcap;
} ckinfo;

static unsigned long ck_strhash(const char *s)
{
  unsigned long h=2166136261UL;
  while (*s!='\0')
    h=(h^(unsigned char)*s++)*16777619UL;
  return h;
}

static int cmp_str(const void *a,const void *b)
{
  return strcmp(*(char*const*)a,*(char*const*)b);
}

/* identity of a symbol across states: name, kind, class, file of a static, and the number of
 * earlier symbols with all of these equal */
static char **ck_keys(int *count)
{
  symbol *sym;
  char **keys;
  int n=0,i;
  for (sym=glbtab.next; sym!=NULL; sym=sym->next)
    sym->work=++n;
  if ((keys=(char**)calloc((size_t)n+1,sizeof(char*)))==NULL)
    error(103);
  {
    /* duplicates: a small open hash of the plain key */
    int cap=1;
    char **seen;
    int *dup;
    while (cap<2*n+2)
      cap*=2;
    seen=(char**)calloc((size_t)cap,sizeof(char*));
    dup=(int*)calloc((size_t)cap,sizeof(int));
    if (seen==NULL || dup==NULL)
      error(103);
    for (sym=glbtab.next, i=0; sym!=NULL; sym=sym->next, i++) {
      char base[sNAMEMAX+64],full[sNAMEMAX+80];
      unsigned long h;
      int slot;
      sprintf(base,"%s|%d|%d|%d",sym->name,sym->ident,sym->vclass,sym->fnumber);
      h=ck_strhash(base);
      for (slot=(int)(h&(unsigned long)(cap-1)); seen[slot]!=NULL && strcmp(seen[slot],base)!=0; slot=(slot+1)&(cap-1))
        /* nothing */;
      if (seen[slot]==NULL)
        seen[slot]=duplicatestring(base);
      sprintf(full,"%s|%d",base,dup[slot]++);
      keys[i]=duplicatestring(full);
    } /* for */
    for (i=0; i<cap; i++)
      free(seen[i]);
    free(seen);
    free(dup);
  }
  *count=n;
  return keys;
}

static int ck_find(const ckinfo *ci,const char *key)
{
  int slot;
  if (ci->htcap==0)
    return -1;
  for (slot=(int)(ck_strhash(key)&(unsigned long)(ci->htcap-1)); ci->ht[slot]>=0; slot=(slot+1)&(ci->htcap-1))
    if (strcmp(ci->syms[ci->ht[slot]].key,key)==0)
      return ci->ht[slot];
  return -1;
}

/* takes what relocation needs from the state loaded now */
static void ck_capture(ckinfo *ci)
{
  symbol *sym;
  char **keys;
  int n,i,k;
  memset(ci,0,sizeof(ckinfo));
  ci->code_idx=code_idx;
  ci->glb_declared=glb_declared;
  ci->labnum=sc_labnum;
  ci->errnum=errnum;
  ci->warnnum=warnnum;
  ckpt_sc5_get(ci->msg);
  {
    valuepair *hp;
    for (hp=heaplist_first(); hp!=NULL; hp=hp->next)
      ci->nheap++;
    if ((ci->heap=(long*)malloc(((size_t)ci->nheap+1)*2*sizeof(long)))==NULL)
      error(103);
    for (hp=heaplist_first(), i=0; hp!=NULL; hp=hp->next, i++) {
      ci->heap[2*i]=hp->first;
      ci->heap[2*i+1]=hp->second;
    } /* for */
  }
  ci->nlev=ckpt_level_count();
  if (ci->nlev>(int)arraysize(ci->lev_f))
    ci->nlev=(int)arraysize(ci->lev_f);
  for (i=0; i<ci->nlev; i++) {
    FILE *h;
    ckpt_level_get(i,&ci->lev_f[i],&ci->lev_line[i],&h);
    ci->lev_pos[i]=(h!=NULL) ? pc_srcpos(h,NULL) : 0;
  } /* for */
  keys=ck_keys(&n);
  ci->nsym=n;
  if ((ci->syms=(cksym*)calloc((size_t)n+1,sizeof(cksym)))==NULL)
    error(103);
  ci->htcap=1;
  while (ci->htcap<2*n+2)
    ci->htcap*=2;
  if ((ci->ht=(int*)malloc((size_t)ci->htcap*sizeof(int)))==NULL)
    error(103);
  for (i=0; i<ci->htcap; i++)
    ci->ht[i]=-1;
  for (sym=glbtab.next, i=0; sym!=NULL; sym=sym->next, i++) {
    cksym *c=&ci->syms[i];
    int slot;
    c->key=keys[i];
    c->addr=sym->addr;
    c->codeaddr=sym->codeaddr;
    c->usage=(unsigned short)sym->usage;
    c->lnumber=sym->lnumber;
    c->lfile=sym->lfile;
    c->stacksize=(sym->ident==iFUNCTN && (sym->usage & uNATIVE)==0) ? sym->x.stacksize : 0;
    if ((c->refs=(char**)malloc(((size_t)sym->numrefers+1)*sizeof(char*)))==NULL)
      error(103);
    for (k=0; k<sym->numrefers; k++)
      if (sym->refer[k]!=NULL && sym->refer[k]->work>0 && sym->refer[k]->work<=n)
        c->refs[c->nrefs++]=keys[sym->refer[k]->work-1];
    qsort(c->refs,(size_t)c->nrefs,sizeof(char*),cmp_str);
    if (sym->states!=NULL) {
      constvalue *cv;
      for (cv=sym->states->first; cv!=NULL; cv=cv->next)
        c->nstates++;
      if ((c->statev=(cell*)calloc((size_t)c->nstates+1,sizeof(cell)))==NULL)
        error(103);
      for (k=0, cv=sym->states->first; cv!=NULL; cv=cv->next, k++)
        c->statev[k]=cv->value;
    } /* if */
    if (sym->ident==iFUNCTN && sym->dim.arglist!=NULL) {
      arginfo *arg;
      for (arg=sym->dim.arglist; arg->ident!=0; arg++)
        c->nargs++;
      if ((c->argaddr=(cell*)calloc((size_t)c->nargs+1,sizeof(cell)))==NULL)
        error(103);
      for (k=0, arg=sym->dim.arglist; arg->ident!=0; arg++, k++)
        if (arg->ident==iREFARRAY && arg->hasdefault)
          c->argaddr[k]=arg->defvalue.array.addr;
    } /* if */
    for (slot=(int)(ck_strhash(c->key)&(unsigned long)(ci->htcap-1)); ci->ht[slot]>=0; slot=(slot+1)&(ci->htcap-1))
      /* nothing */;
    ci->ht[slot]=i;
  } /* for */
  for (sym=glbtab.next; sym!=NULL; sym=sym->next)
    sym->work=0;
  free(keys);           /* the strings live on in the records */
}

static int ck_hasref(const cksym *c,const char *key)
{
  return c!=NULL && bsearch(&key,c->refs,(size_t)c->nrefs,sizeof(char*),cmp_str)!=NULL;
}

static int ck_linedelta(const ckinfo *a,const ckinfo *b,int fnum,int *dline,long *dpos)
{
  int i;
  for (i=0; i<a->nlev && i<b->nlev; i++) {
    if (a->lev_f[i]!=b->lev_f[i])
      break;
    if (a->lev_f[i]==fnum) {
      *dline=b->lev_line[i]-a->lev_line[i];
      *dpos=(long)b->lev_pos[i]-(long)a->lev_pos[i];
      return TRUE;
    } /* if */
  } /* for */
  return FALSE;
}

/* applies b - a to the state loaded now */
static void ck_relocate(const ckinfo *a,const ckinfo *b)
{
  cell dcode=b->code_idx-a->code_idx;
  cell ddata=(b->glb_declared-a->glb_declared)*(cell)sizeof(cell);
  symbol *sym;
  char **keys;
  int n,i,k;

  /* a code or data address that this state set after <a> lies between the counters of <a> and the
   * counters of this state; values outside (from the first pass, say) stay */
  cell c_lo=a->code_idx,c_hi=code_idx;
  cell d_lo=a->glb_declared*(cell)sizeof(cell),d_hi=glb_declared*(cell)sizeof(cell);
  #define CK_IN(v,lo,hi)  ((v)>=(lo) && (v)<=(hi))

  code_idx+=dcode;
  glb_declared+=b->glb_declared-a->glb_declared;
  sc_labnum+=b->labnum-a->labnum;
  errnum+=b->errnum-a->errnum;
  warnnum+=b->warnnum-a->warnnum;
  {
    /* the heap list: the first pass appends one entry per function, the second pass takes them from
     * the front; so this state's list either starts with <a>'s (a later first-pass point) or is
     * the end of it (a later second-pass point) */
    valuepair *hp;
    long *cur;
    int ncur=0,m;
    for (hp=heaplist_first(); hp!=NULL; hp=hp->next)
      ncur++;
    if ((cur=(long*)malloc(((size_t)ncur+1)*2*sizeof(long)))==NULL)
      error(103);
    for (hp=heaplist_first(), m=0; hp!=NULL; hp=hp->next, m++) {
      cur[2*m]=hp->first;
      cur[2*m+1]=hp->second;
    } /* for */
    if (a->nheap!=b->nheap || memcmp(a->heap,b->heap,(size_t)a->nheap*2*sizeof(long))!=0) {
      if (ncur>=a->nheap && memcmp(cur,a->heap,(size_t)a->nheap*2*sizeof(long))==0) {
        delete_heaplisttable();
        for (m=0; m<b->nheap; m++)
          push_heaplist(b->heap[2*m],b->heap[2*m+1]);
        for (m=a->nheap; m<ncur; m++)
          push_heaplist(cur[2*m],cur[2*m+1]);
      } else if (ncur<=a->nheap && a->nheap==b->nheap
                 && memcmp(cur,a->heap+2*(a->nheap-ncur),(size_t)ncur*2*sizeof(long))==0) {
        delete_heaplisttable();
        for (m=b->nheap-ncur; m<b->nheap; m++)
          push_heaplist(b->heap[2*m],b->heap[2*m+1]);
      } /* if */
    } /* if */
    free(cur);
  }
  {
    /* the message state: what segment j left, unless a later message changed it again */
    int v[7],m;
    ckpt_sc5_get(v);
    for (m=0; m<7; m++)
      if (v[m]==a->msg[m])
        v[m]=b->msg[m];
    ckpt_sc5_set(v);
  }

  /* include levels: a file whose position at the boundary moved */
  for (i=0; i<ckpt_level_count(); i++) {
    int fnum,line,dline;
    long dpos;
    FILE *h;
    ckpt_level_get(i,&fnum,&line,&h);
    if (ck_linedelta(a,b,fnum,&dline,&dpos)) {
      ckpt_level_setline(i,line+dline);
      if (h!=NULL) {
        int eof;
        size_t pos=pc_srcpos(h,&eof);
        pc_srcsetpos(h,(size_t)((long)pos+dpos),eof);
      } /* if */
    } /* if */
  } /* for */
  if (line_sym!=NULL)
    line_sym->addr=fline;

  keys=ck_keys(&n);
  for (sym=glbtab.next, i=0; sym!=NULL; sym=sym->next, i++) {
    int ia=ck_find(a,keys[i]),ib=ck_find(b,keys[i]);
    const cksym *ca=(ia>=0) ? &a->syms[ia] : NULL;
    const cksym *cb=(ib>=0) ? &b->syms[ib] : NULL;
    int isfunc=(sym->ident==iFUNCTN && (sym->usage & uNATIVE)==0);
    int isvar=((sym->ident==iVARIABLE || sym->ident==iARRAY) && sym->vclass!=sLOCAL);
    int dline;
    long dpos;
    if (ca!=NULL && cb!=NULL) {
      sym->usage=(short)(unsigned short)(((unsigned short)sym->usage) ^ (ca->usage ^ cb->usage));
      if (sym->addr==ca->addr)
        sym->addr=cb->addr;
      else if (isfunc && CK_IN(sym->addr,c_lo,c_hi))
        sym->addr+=dcode;
      else if (isvar && CK_IN(sym->addr,d_lo,d_hi))
        sym->addr+=ddata;
      if (sym->codeaddr==ca->codeaddr)
        sym->codeaddr=cb->codeaddr;
      else if (CK_IN(sym->codeaddr,c_lo,c_hi))
        sym->codeaddr+=dcode;
      if (sym->lnumber==ca->lnumber && sym->lfile==ca->lfile) {
        sym->lnumber=cb->lnumber;
        sym->lfile=cb->lfile;
      } else if (ck_linedelta(a,b,sym->lfile,&dline,&dpos)) {
        sym->lnumber+=dline;
      } /* if */
      if (isfunc && sym->x.stacksize==ca->stacksize)
        sym->x.stacksize=cb->stacksize;
      if (sym->ident==iFUNCTN && sym->dim.arglist!=NULL) {
        /* default arrays of arguments live in the data segment, put there by the declaration */
        arginfo *arg;
        for (k=0, arg=sym->dim.arglist; arg->ident!=0; arg++, k++) {
          if (arg->ident!=iREFARRAY || !arg->hasdefault)
            continue;
          if (k<ca->nargs && k<cb->nargs && arg->defvalue.array.addr==ca->argaddr[k])
            arg->defvalue.array.addr=cb->argaddr[k];
          else if (CK_IN(arg->defvalue.array.addr,d_lo,d_hi))
            arg->defvalue.array.addr+=ddata;
        } /* for */
      } /* if */
    } else if (ca==NULL) {
      /* made after the start of segment j+1 */
      if (CK_IN(sym->codeaddr,c_lo,c_hi))
        sym->codeaddr+=dcode;
      if (isfunc && CK_IN(sym->addr,c_lo,c_hi))
        sym->addr+=dcode;
      else if (isvar && CK_IN(sym->addr,d_lo,d_hi))
        sym->addr+=ddata;
      if (sym->ident==iFUNCTN && sym->dim.arglist!=NULL) {
        arginfo *arg;
        for (arg=sym->dim.arglist; arg->ident!=0; arg++)
          if (arg->ident==iREFARRAY && arg->hasdefault && CK_IN(arg->defvalue.array.addr,d_lo,d_hi))
            arg->defvalue.array.addr+=ddata;
      } /* if */
      if (ck_linedelta(a,b,sym->lfile,&dline,&dpos))
        sym->lnumber+=dline;
    } /* if */
    if (sym->ident==iFUNCTN && sym->states!=NULL) {
      /* the code address of every state implementation */
      constvalue *cv;
      for (k=0, cv=sym->states->first; cv!=NULL; cv=cv->next, k++) {
        if (ca!=NULL && cb!=NULL && k<ca->nstates && k<cb->nstates && cv->value==ca->statev[k])
          cv->value=cb->statev[k];
        else if (CK_IN(cv->value,c_lo,c_hi))
          cv->value+=dcode;
      } /* for */
    } /* if */
    sym->work=i+1;
  } /* for */

  /* referrers: drop those segment j dropped, add those it added */
  for (sym=glbtab.next, i=0; sym!=NULL; sym=sym->next, i++) {
    int ia=ck_find(a,keys[i]),ib=ck_find(b,keys[i]);
    const cksym *ca=(ia>=0) ? &a->syms[ia] : NULL;
    const cksym *cb=(ib>=0) ? &b->syms[ib] : NULL;
    if (ca==NULL || cb==NULL)
      continue;
    for (k=0; k<sym->numrefers; k++) {
      if (sym->refer[k]!=NULL && sym->refer[k]->work>0) {
        const char *rk=keys[sym->refer[k]->work-1];
        if (ck_hasref(ca,rk) && !ck_hasref(cb,rk))
          sym->refer[k]=NULL;
      } /* if */
    } /* for */
    for (k=0; k<cb->nrefs; k++) {
      if (!ck_hasref(ca,cb->refs[k])) {
        /* find the referrer by its key in this state */
        symbol *r;
        int j;
        for (r=glbtab.next, j=0; r!=NULL; r=r->next, j++)
          if (strcmp(keys[j],cb->refs[k])==0)
            break;
        if (r!=NULL)
          refer_symbol(sym,r);
      } /* if */
    } /* for */
  } /* for */
  for (sym=glbtab.next; sym!=NULL; sym=sym->next)
    sym->work=0;
  for (i=0; i<n; i++)
    free(keys[i]);
  free(keys);
}

static void ck_loadinfo(const char *path,ckinfo *ci)
{
  if (!ck_load(path)) {
    pc_printf("[ckpt] cannot load %s\n",path);
    exit(4);
  } /* if */
  ck_capture(ci);
}

/* PAWNCC_SEG_LOAD: put the state of a checkpoint in place; parse() continues from there */
SC_FUNC int ckpt_load_start(void)
{
  const char *path=getenv("PAWNCC_SEG_LOAD");
  const char *ra=getenv("PAWNCC_SEG_RELOC_A");
  const char *rb=getenv("PAWNCC_SEG_RELOC_B");
  ckinfo ia,ib;
  int reloc=(ra!=NULL && *ra!='\0' && rb!=NULL && *rb!='\0');
  if (path==NULL || *path=='\0')
    return FALSE;
  if (reloc) {
    ck_loadinfo(rb,&ib);
    ck_loadinfo(ra,&ia);
  } /* if */
  if (!ck_load(path)) {
    pc_printf("[ckpt] cannot load %s\n",path);
    exit(4);
  } /* if */
  if (reloc) {
    ck_relocate(&ia,&ib);   /* the records are not freed: the process ends with the segment */
    if (ck_out!=NULL || getenv("PAWNCC_SEG_OUT")!=NULL) {
      /* the start this segment really used, for the chain check */
      char name[_MAX_PATH+16];
      sprintf(name,"%s.start.ck",getenv("PAWNCC_SEG_OUT"));
      ck_save(name);
    } /* if */
  } /* if */
  ck_dbgstart=dbgstring_count();
  ckpt_init();
  ck_skip_next=1;
  ck_files_reset();
  if (getenv("PAWNCC_CKPT_RESAVE")!=NULL) {
    /* test: a loaded checkpoint saved again must give the same file */
    exit(ck_save(getenv("PAWNCC_CKPT_RESAVE")) ? 0 : 3);
  } /* if */
  return TRUE;
}

/* a pass ran to its end (second pass: after writetrailer and testsymbols) in a segment */
SC_FUNC void ckpt_pass_end(void)
{
  char name[_MAX_PATH+16];
  int ok;
  if (!ckpt_active() || ck_out==NULL)
    return;
  sprintf(name,"%s.end.ck",ck_out);
  ok=ck_save(name);
  ck_write_outputs();
  ck_write_done(ok);
  fflush(stdout);
  fflush(stderr);
  exit(ok ? 0 : 3);
}

/* PAWNCC_ASM_FROM: load the final state and the pieces; the caller then assembles */
SC_FUNC int ckpt_assemble_input(void)
{
  const char *list=getenv("PAWNCC_ASM_FROM");
  char path[_MAX_PATH+16],*p;
  FILE *lf,*f;
  int first=TRUE;
  static char line[sLINEMAX+2];
  if (list==NULL || *list=='\0')
    return FALSE;
  if ((lf=fopen(list,"rb"))==NULL) {
    pc_printf("[ckpt] cannot read %s\n",list);
    exit(4);
  } /* if */
  while (fgets(path,sizeof path,lf)!=NULL) {
    size_t n=strlen(path);
    while (n>0 && (path[n-1]=='\n' || path[n-1]=='\r'))
      path[--n]='\0';
    if (n==0)
      continue;
    if (first) {
      first=FALSE;
      if (!ck_load(path)) {
        pc_printf("[ckpt] cannot load %s\n",path);
        exit(4);
      } /* if */
      delete_dbgstringtable();
      continue;
    } /* if */
    if ((f=fopen(path,"rb"))==NULL) {
      pc_printf("[ckpt] cannot read %s\n",path);
      exit(4);
    } /* if */
    setvbuf(f,NULL,_IOFBF,1<<20);
    p=strrchr(path,'.');
    if (p!=NULL && strcmp(p,".dbg")==0) {
      /* one block per piece, the lines split in place (the table borrows them) */
      long size;
      char *buf,*q,*end,**strs;
      int count=0,cap=1024;
      fseek(f,0,SEEK_END);
      size=ftell(f);
      fseek(f,0,SEEK_SET);
      if ((buf=(char*)malloc((size_t)size+1))==NULL || (strs=(char**)malloc((size_t)cap*sizeof(char*)))==NULL)
        error(103);
      if (fread(buf,1,(size_t)size,f)!=(size_t)size) {
        pc_printf("[ckpt] cannot read %s\n",path);
        exit(4);
      } /* if */
      buf[size]='\0';
      for (q=buf, end=buf+size; q<end; ) {
        char *nlp=(char*)memchr(q,'\n',(size_t)(end-q));
        char *e=(nlp!=NULL) ? nlp : end;
        char *t=e;
        while (t>q && t[-1]=='\r')
          t--;
        *t='\0';
        if (count==cap) {
          cap*=2;
          if ((strs=(char**)realloc(strs,(size_t)cap*sizeof(char*)))==NULL)
            error(103);
        } /* if */
        strs[count++]=q;
        q=(nlp!=NULL) ? nlp+1 : end;
      } /* for */
      dbgstring_append_borrowed(strs,count);
      free(strs);           /* buf stays: the table points into it */
    } else {
      /* the whole piece in one write */
      long size;
      char *buf;
      fseek(f,0,SEEK_END);
      size=ftell(f);
      fseek(f,0,SEEK_SET);
      if (size>0) {
        if ((buf=(char*)malloc((size_t)size+1))==NULL)
          error(103);
        if (fread(buf,1,(size_t)size,f)!=(size_t)size) {
          pc_printf("[ckpt] cannot read %s\n",path);
          exit(4);
        } /* if */
        buf[size]='\0';
        pc_writeasm(outf,buf);
        free(buf);
      } /* if */
    } /* if */
    fclose(f);
  } /* while */
  fclose(lf);
  return TRUE;
}
