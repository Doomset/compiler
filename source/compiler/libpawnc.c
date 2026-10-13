/*  LIBPAWNC.C
 *
 *  A "glue file" for building the Pawn compiler as a DLL or shared library.
 *
 *  Copyright (c) ITB CompuPhase, 2000-2006
 *
 *  This software is provided "as-is", without any express or implied warranty.
 *  In no event will the authors be held liable for any damages arising from
 *  the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1.  The origin of this software must not be misrepresented; you must not
 *      claim that you wrote the original software. If you use this software in
 *      a product, an acknowledgment in the product documentation would be
 *      appreciated but is not required.
 *  2.  Altered source versions must be plainly marked as such, and must not be
 *      misrepresented as being the original software.
 *  3.  This notice may not be removed or altered from any source distribution.
 *
 *  Version: $Id: libpawnc.c 3612 2006-07-22 09:59:46Z thiadmer $
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#if defined _WIN32
  #include <windows.h>
#endif
#include "sc.h"

#if defined PAWNC_DLL

# include "../amx/dllmain.c"

# define MAX_ARGS   100
# if !defined UNUSED_PARAM
#   define UNUSED_PARAM(p) ((void)(p))
# endif

  static char *argv[MAX_ARGS];
  static int  argc;

  LPSTR dll_skipwhite(LPSTR ptr)
  {
    assert(ptr!=NULL);
    while (*ptr<=' ' && *ptr!='\0')
      ptr++;
    return ptr;
  }

# if defined __WIN32__ || defined _WIN32 || defined WIN32 || defined __NT__
  __declspec (dllexport)
  void CALLBACK Compile(HWND hwnd, HINSTANCE hinst, LPSTR lpCommandLine, int nCmdShow)
# else
  void FAR PASCAL __export Compile(HWND hwnd, HINSTANCE hinst, LPSTR lpCommandLine, int nCmdShow)
# endif
  {
    char RootPath[_MAX_PATH];
    LPSTR ptr;

    /* RUNDLL32 may have passed us a HWND and a HINSTANCE, but we can hardly
     * trust these. They may not contain values that we can use.
     */

    /* the root path in argv[0] */
    GetModuleFileName(hinstDLL, RootPath, sizeof RootPath);
    argv[argc++]=RootPath;

    /* all other options */
    assert(lpCommandLine!=NULL);
    ptr=dll_skipwhite(lpCommandLine);
    while (*ptr!='\0') {
      if (*ptr=='"') {
        argv[argc++]=ptr+1;
        while (*ptr!='"' && *ptr!='\0')
          ptr++;
      } else {
        argv[argc++]=ptr;
        while (*ptr>' ')
          ptr++;
      } /* if */
      if (*ptr!='\0')
        *ptr++='\0';
      ptr=dll_skipwhite(ptr);
    } /* while */
    pc_compile(argc,argv);
    UNUSED_PARAM(hwnd);
    UNUSED_PARAM(hinst);
    UNUSED_PARAM(nCmdShow);
  }

#else /* PAWNC_DLL */


#endif /* PAWNC_DLL */


/* pc_printf()
 * Called for general purpose "console" output. This function prints general
 * purpose messages; errors go through pc_error(). The function is modelled
 * after printf().
 */
int pc_printf(const char *message,...)
{
  int ret;
  va_list argptr;

  va_start(argptr,message);
  ret=vprintf(message,argptr);
  va_end(argptr);

  return ret;
}

/* pc_error()
 * Called for producing error output.
 *    number      the error number (as documented in the manual)
 *    message     a string describing the error with embedded %d and %s tokens
 *    filename    the name of the file currently being parsed
 *    firstline   the line number at which the expression started on which
 *                the error was found, or -1 if there is no "starting line"
 *    lastline    the line number at which the error was detected
 *    argptr      a pointer to the first of a series of arguments (for macro
 *                "va_arg")
 * Return:
 *    If the function returns 0, the parser attempts to continue compilation.
 *    On a non-zero return value, the parser aborts.
 */
int pc_error(int number,char *message,char *filename,int firstline,int lastline,va_list argptr)
{
  static char *prefix[3]={ "error", "fatal error", "warning" };

  if (number!=0) {
    char *pre;

    pre=prefix[number/100];
    if (number>=200 && pc_geterrorwarnings()){
      pre=prefix[0];
    }
    if (number == 111 || number == 237)
      fprintf(stderr,"%s(%d) : ",filename,lastline);
    else if (firstline>=0)
      fprintf(stderr,"%s(%d -- %d) : %s %03d: ",filename,firstline,lastline,pre,number);
    else
      fprintf(stderr,"%s(%d) : %s %03d: ",filename,lastline,pre,number);
  } /* if */
  vfprintf(stderr,message,argptr);
  fflush(stderr);
  return 0;
}

/* pc_opensrc()
 * Opens a source file (or include file) for reading. The "file" does not have
 * to be a physical file, one might compile from memory.
 *    filename    the name of the "file" to read from
 * Return:
 *    The function must return a pointer, which is used as a "magic cookie" to
 *    all I/O functions. When failing to open the file for reading, the
 *    function must return NULL.
 * Note:
 *    Several "source files" may be open at the same time. Specifically, one
 *    file can be open for reading and another for writing.
 */
/* Source files are loaded into memory once and kept for the whole run: every pass, every re-included file
 * and scan_utf8() read the same bytes, and fgets() on a FILE* (lock, per-line CRT work) was ~10% of a compile.
 * Semantics follow a text-mode fopen(): "\r\n" becomes "\n" and ^Z ends the data.
 */
typedef struct tagSRCFILE {
  FILE *fp;                     /* write handle (pc_createsrc/pc_createtmpsrc), NULL for a read handle */
  const char *data;
  size_t size,pos;
  int eof;                      /* set like feof(): only after a read ran into the end of the data */
} SRCFILE;

typedef struct tagSRCCACHE {
  struct tagSRCCACHE *next;
  char *data;
  size_t size;
  char name[1];
} SRCCACHE;
#define SRCCACHE_BUCKETS 4096
static SRCCACHE *srccache[SRCCACHE_BUCKETS];

static unsigned srccache_hash(const char *s)
{
  unsigned h=5381;
  while (*s!='\0')
    h=h*33u+(unsigned char)*s++;
  return h&(SRCCACHE_BUCKETS-1);
}

#ifdef PERF_TRACE
static unsigned long sc_hits=0,sc_loads=0,sc_statfail=0,sc_dirmiss=0;
static double sc_bytes=0,sc_t_stat=0,sc_t_open=0,sc_t_read=0,sc_t_xlat=0;
static void srccache_report(void)
{
  fprintf(stderr,"[perf] srccache: hits=%lu loads=%lu dir_miss=%lu stat_fail=%lu bytes=%.1f MB | miss+stat %.3f s, fopen %.3f s, read+close %.3f s, text xlat %.3f s\n",
          sc_hits,sc_loads,sc_dirmiss,sc_statfail,sc_bytes/1048576.0,sc_t_stat,sc_t_open,sc_t_read,sc_t_xlat);
}
#endif
#if defined _WIN32
/* Include search tries every include directory with every extension: ~14,000 paths that do not exist,
 * each a stat() of ~50 us. A directory is listed once instead; a name that is not in the listing of its
 * directory does not exist. Only a definite "missing" is answered here: names the listing cannot judge
 * exactly (non-ASCII, wildcards, 8.3 short names, streams, trailing dots or spaces, device names)
 * and directories that cannot be listed fall back to stat().
 */
typedef struct tagDIRCACHE {
  struct tagDIRCACHE *next;
  int state;                    /* 1 listed, 0 the directory does not exist, -1 unknown (use stat) */
  char **names;                 /* ASCII-lower-case names, sorted */
  size_t count;
  char dir[1];
} DIRCACHE;
#define DIRCACHE_BUCKETS 1024
static DIRCACHE *dircache[DIRCACHE_BUCKETS];

static void ascii_lower(char *s)
{
  for ( ; *s!='\0'; s++)
    if (*s>='A' && *s<='Z')
      *s+='a'-'A';
}

static int dircache_cmp(const void *a,const void *b)
{
  return strcmp(*(char * const *)a,*(char * const *)b);
}

static DIRCACHE *dircache_get(const char *dir,size_t dirlen)
{
  unsigned h=5381;
  size_t i,cap;
  DIRCACHE *d;
  char *pattern;
  WIN32_FIND_DATAA fd;
  HANDLE hf;
  DWORD err;

  for (i=0; i<dirlen; i++)
    h=h*33u+(unsigned char)dir[i];
  h&=DIRCACHE_BUCKETS-1;
  for (d=dircache[h]; d!=NULL; d=d->next)
    if (strlen(d->dir)==dirlen && memcmp(d->dir,dir,dirlen)==0)
      return d;
  if ((d=(DIRCACHE *)calloc(1,sizeof(DIRCACHE)+dirlen))==NULL)
    return NULL;
  memcpy(d->dir,dir,dirlen);
  d->dir[dirlen]='\0';
  d->state=-1;
  if ((pattern=(char *)malloc(dirlen+3))!=NULL) {
    if (dirlen==0) {
      strcpy(pattern,"*");
    } else {
      memcpy(pattern,dir,dirlen);
      strcpy(pattern+dirlen,"\\*");
    } /* if */
    hf=FindFirstFileExA(pattern,FindExInfoBasic,&fd,FindExSearchNameMatch,NULL,FIND_FIRST_EX_LARGE_FETCH);
    if (hf==INVALID_HANDLE_VALUE) {
      err=GetLastError();
      if (err==ERROR_PATH_NOT_FOUND)
        d->state=0;
    } else {
      cap=0;
      d->state=1;
      do {
        if (d->count==cap) {
          char **grown;
          cap=(cap==0) ? 64 : cap*2;
          if ((grown=(char **)realloc(d->names,cap*sizeof(char *)))==NULL) {
            d->state=-1;
            break;
          } /* if */
          d->names=grown;
        } /* if */
        if ((d->names[d->count]=_strdup(fd.cFileName))==NULL) {
          d->state=-1;
          break;
        } /* if */
        ascii_lower(d->names[d->count]);
        d->count++;
      } while (FindNextFileA(hf,&fd));
      if (d->state==1 && GetLastError()!=ERROR_NO_MORE_FILES)
        d->state=-1;
      FindClose(hf);
      if (d->state==1)
        qsort(d->names,d->count,sizeof(char *),dircache_cmp);
    } /* if */
    free(pattern);
  } /* if */
  d->next=dircache[h];
  dircache[h]=d;
  return d;
}

/* TRUE only if "filename" certainly does not exist */
static int dircache_missing(const char *filename)
{
  const char *base,*p;
  char key[_MAX_PATH],*k;
  DIRCACHE *d;

  base=filename;
  for (p=filename; *p!='\0'; p++)
    if (*p=='\\' || *p=='/')
      base=p+1;
  if (*base=='\0' || strlen(base)>=sizeof key)
    return FALSE;
  for (p=base; *p!='\0'; p++)
    if ((unsigned char)*p>=0x80 || *p=='*' || *p=='?' || *p=='~' || *p==':' || *p=='"' || *p=='<' || *p=='>' || *p=='|')
      return FALSE;
  if (p[-1]=='.' || p[-1]==' ')
    return FALSE;               /* Windows strips trailing dots and spaces */
  strcpy(key,base);
  ascii_lower(key);
  for (k=key; *k!='\0' && *k!='.'; k++)
    /* nothing */;
  if (k-key==3 && (strncmp(key,"con",3)==0 || strncmp(key,"prn",3)==0 || strncmp(key,"aux",3)==0 || strncmp(key,"nul",3)==0))
    return FALSE;               /* device names exist everywhere */
  if (k-key==4 && (strncmp(key,"com",3)==0 || strncmp(key,"lpt",3)==0))
    return FALSE;
  if (base!=filename && (base==filename+1 || base[-2]==':' || base[-2]=='\\' || base[-2]=='/'))
    return FALSE;               /* "\x", "C:\x", "a\\x": leave roots and doubled separators to stat() */
  d=dircache_get(filename,(base==filename) ? 0 : (size_t)(base-filename-1));
  if (d==NULL || d->state<0)
    return FALSE;
  if (d->state==0)
    return TRUE;
  k=key;
  return bsearch(&k,d->names,d->count,sizeof(char *),dircache_cmp)==NULL;
}
#endif

static SRCCACHE *srccache_load(const char *filename)
{
  unsigned h=srccache_hash(filename);
  SRCCACHE *e;
  FILE *fp;
  struct stat st;
  long len;
  char *raw;
  size_t n,i,o;

#ifdef PERF_TRACE
  static int reg=0;
  double t0,t1;
  if (!reg) { reg=1; atexit(srccache_report); }
#endif
  for (e=srccache[h]; e!=NULL; e=e->next)
    if (strcmp(e->name,filename)==0) {
#ifdef PERF_TRACE
      sc_hits++;
#endif
      return e;
    } /* if */
#ifdef PERF_TRACE
  t0=perf_now();
#endif
#if defined _WIN32
  if (dircache_missing(filename)) {
#ifdef PERF_TRACE
    sc_dirmiss++;
    sc_t_stat+=perf_now()-t0;
#endif
    return NULL;
  } /* if */
#endif
  if (stat(filename,&st)!=0 || (st.st_mode & S_IFDIR)!=0) {
#ifdef PERF_TRACE
    if (getenv("PAWNCC_TRACE_STATFAIL")!=NULL) fprintf(stderr,"[statfail] %s\n",filename);
    sc_statfail++;
    sc_t_stat+=perf_now()-t0;
#endif
    return NULL;                /* no such file; a directory with the same name is not a source file */
  } /* if */
#ifdef PERF_TRACE
  t1=perf_now(); sc_t_stat+=t1-t0; t0=t1;
#endif
  if ((fp=fopen(filename,"rb"))==NULL)
    return NULL;
  fseek(fp,0L,SEEK_END);
  len=ftell(fp);
  fseek(fp,0L,SEEK_SET);
#ifdef PERF_TRACE
  t1=perf_now(); sc_t_open+=t1-t0; t0=t1;
#endif
  if (len<0 || (raw=(char *)malloc((size_t)len+1))==NULL) {
    fclose(fp);
    return NULL;
  } /* if */
  n=fread(raw,1,(size_t)len,fp);
  fclose(fp);
#ifdef PERF_TRACE
  t1=perf_now(); sc_t_read+=t1-t0; t0=t1;
  sc_loads++; sc_bytes+=(double)n;
#endif
  /* text-mode translation, in place */
  for (i=o=0; i<n; i++) {
    if (raw[i]=='\x1a')
      break;
    if (raw[i]=='\r' && i+1<n && raw[i+1]=='\n')
      continue;
    raw[o++]=raw[i];
  } /* for */
  raw[o]='\0';
#ifdef PERF_TRACE
  sc_t_xlat+=perf_now()-t0;
#endif
  if ((e=(SRCCACHE *)malloc(sizeof(SRCCACHE)+strlen(filename)))==NULL) {
    free(raw);
    return NULL;
  } /* if */
  strcpy(e->name,filename);
  e->data=raw;
  e->size=o;
  e->next=srccache[h];
  srccache[h]=e;
  return e;
}

void *pc_opensrc(char *filename)
{
  SRCCACHE *e=srccache_load(filename);
  SRCFILE *f;

  if (e==NULL || (f=(SRCFILE *)calloc(1,sizeof(SRCFILE)))==NULL)
    return NULL;
  f->data=e->data;
  f->size=e->size;
  return f;
}

/* pc_createsrc()
 * Creates/overwrites a source file for writing. The "file" does not have
 * to be a physical file, one might compile from memory.
 *    filename    the name of the "file" to create
 * Return:
 *    The function must return a pointer, which is used as a "magic cookie" to
 *    all I/O functions. When failing to open the file for reading, the
 *    function must return NULL.
 * Note:
 *    Several "source files" may be open at the same time. Specifically, one
 *    file can be open for reading and another for writing.
 */
static void *srcfile_wrap(FILE *fp)
{
  SRCFILE *f;

  if (fp==NULL || (f=(SRCFILE *)calloc(1,sizeof(SRCFILE)))==NULL)
    return NULL;
  f->fp=fp;
  return f;
}

void *pc_createsrc(char *filename)
{
  return srcfile_wrap(fopen(filename,"wt"));
}

/* pc_createtmpsrc()
 * Creates a temporary source file with a unique name for writing.
 * Return:
 *    The function must return a pointer, which is used as a "magic cookie" to
 *    all I/O functions. When failing to open the file for reading, the
 *    function must return NULL.
 */
void *pc_createtmpsrc(char **filename)
{
  char *tname=NULL;
  FILE *ftmp=NULL;

  #if defined	__WIN32__ || defined _WIN32
    tname=_tempnam(NULL,"pawn");
    ftmp=fopen(tname,"wt");
  #elif defined __MSDOS__ || defined _Windows
    tname=tempnam(NULL,"pawn");
    ftmp=fopen(tname,"wt");
  #else
    static const char template[]="/tmp/pawnXXXXXX";
    if ((tname=malloc(sizeof(template)))!=NULL) {
      int fdtmp;
      strncpy(tname,template,sizeof(template));
      if ((fdtmp=mkstemp(tname))>=0)
        ftmp=fdopen(fdtmp,"wt");
      if (fdtmp<0 || filename==NULL) {
        free(tname);
        tname=NULL;
      } /* if */
    } /* if */
  #endif
  if (filename!=NULL)
    *filename=tname;
  return srcfile_wrap(ftmp);
}

/* pc_closesrc()
 * Closes a source file (or include file). The "handle" parameter has the
 * value that pc_opensrc() returned in an earlier call.
 */
void pc_closesrc(void *handle)
{
  SRCFILE *f=(SRCFILE *)handle;

  assert(handle!=NULL);
  if (f->fp!=NULL)
    fclose(f->fp);
  free(f);
}

/* pc_resetsrc()
 * "position" may only hold a pointer that was previously obtained from
 * pc_getpossrc()
 */
void pc_resetsrc(void *handle,void *position)
{
  SRCFILE *f=(SRCFILE *)handle;

  assert(handle!=NULL);
  f->pos=*(size_t *)position;
  f->eof=0;
}

/* pc_readsrc()
 * Reads a single line from the source file (or up to a maximum number of
 * characters if the line in the input file is too long).
 */
char *pc_readsrc(void *handle,unsigned char *target,int maxchars)
{
  SRCFILE *f=(SRCFILE *)handle;
  size_t avail,n;
  const char *nl;

  if (maxchars<=0)
    return NULL;
  avail=f->size-f->pos;
  if (avail==0) {
    f->eof=1;
    return NULL;
  } /* if */
  n=(size_t)(maxchars-1);       /* fgets() stops one short of maxchars */
  if (n==0) {
    *target='\0';
    return (char*)target;
  } /* if */
  if (n>avail)
    n=avail;
  nl=(const char *)memchr(f->data+f->pos,'\n',n);
  if (nl!=NULL)
    n=(size_t)(nl-(f->data+f->pos))+1;
  else if (avail<(size_t)(maxchars-1))
    f->eof=1;                   /* fgets() ran into the end without finding a newline */
  memcpy(target,f->data+f->pos,n);
  target[n]='\0';
  f->pos+=n;
  return (char*)target;
}

/* pc_writesrc()
 * Writes to to the source file. There is no automatic line ending; to end a
 * line, write a "\n".
 */
int pc_writesrc(void *handle,unsigned char *source)
{
  return fputs((char*)source,((SRCFILE *)handle)->fp) >= 0;
}

void *pc_getpossrc(void *handle)
{
  static size_t lastpos;  /* may need to have a LIFO stack of such positions */

  lastpos=((SRCFILE *)handle)->pos;
  return &lastpos;
}

int pc_eofsrc(void *handle)
{
  return ((SRCFILE *)handle)->eof;
}

/* should return a pointer, which is used as a "magic cookie" to all I/O
 * functions; return NULL for failure
 */
void *pc_openasm(char *filename)
{
  #if defined __MSDOS__ || defined SC_LIGHT
    return fopen(filename,"w+t");
  #else
    return mfcreate(filename);
  #endif
}

void pc_closeasm(void *handle, int deletefile)
{
  #if defined __MSDOS__ || defined SC_LIGHT
    if (handle!=NULL)
      fclose((FILE*)handle);
    if (deletefile)
      remove(outfname);
  #else
    if (handle!=NULL) {
      if (!deletefile)
        mfdump((MEMFILE*)handle);
      mfclose((MEMFILE*)handle);
    } /* if */
  #endif
}

void pc_resetasm(void *handle)
{
  assert(handle!=NULL);
  #if defined __MSDOS__ || defined SC_LIGHT
    fflush((FILE*)handle);
    fseek((FILE*)handle,0,SEEK_SET);
  #else
    mfseek((MEMFILE*)handle,0,SEEK_SET);
  #endif
}

long pc_seekasm(void *handle,long offset)
{
  #if defined __MSDOS__ || defined SC_LIGHT
    fseek((FILE*)handle,offset,SEEK_SET);
  #else
    mfseek((MEMFILE*)handle,offset,SEEK_SET);
  #endif
  return offset;
}

int pc_writeasm(void *handle,char *string)
{
  #if defined __MSDOS__ || defined SC_LIGHT
    return fputs(string,(FILE*)handle) >= 0;
  #else
    return mfputs((MEMFILE*)handle,string);
  #endif
}

char *pc_readasm(void *handle, char *string, int maxchars)
{
  #if defined __MSDOS__ || defined SC_LIGHT
    return fgets(string,maxchars,(FILE*)handle);
  #else
    return mfgets((MEMFILE*)handle,string,maxchars);
  #endif
}

/* Should return a pointer, which is used as a "magic cookie" to all I/O
 * functions; return NULL for failure.
 */
void *pc_openbin(char *filename)
{
  FILE *fbin;

  fbin=fopen(filename,"wb");
  setvbuf(fbin,NULL,_IOFBF,1UL<<20);
  return fbin;
}

/* The AMX is written one small piece per cell (15 million calls for a large gamemode). Pieces are collected in a
 * buffer of our own; every other operation on the binary file flushes it first.
 */
static unsigned char binbuf[1<<16];
static size_t binbuf_len=0;

static void binbuf_flush(FILE *fp)
{
  if (binbuf_len>0) {
    fwrite(binbuf,1,binbuf_len,fp);
    binbuf_len=0;
  } /* if */
}

void pc_closebin(void *handle,int deletefile)
{
  binbuf_flush((FILE*)handle);
  fclose((FILE*)handle);
  if (deletefile)
    remove(binfname);
}

/* pc_resetbin()
 * Can seek to any location in the file.
 * The offset is always from the start of the file.
 */
void pc_resetbin(void *handle,long offset)
{
  binbuf_flush((FILE*)handle);
  fflush((FILE*)handle);
  fseek((FILE*)handle,offset,SEEK_SET);
}

int pc_writebin(void *handle,void *buffer,int size)
{
  /* The compiler is single-threaded: the per-call stream lock of fwrite() was ~8% of a large compile
   * (one call per cell of the AMX). */
  if (size>0 && (size_t)size<=sizeof binbuf) {
    if (binbuf_len+(size_t)size>sizeof binbuf) {
      /* a failed write is reported by the call that hits it, like with an unbuffered stream */
      size_t written=fwrite(binbuf,1,binbuf_len,(FILE*)handle);
      int ok=(written==binbuf_len);
      binbuf_len=0;
      if (!ok)
        return 0;
    } /* if */
    memcpy(binbuf+binbuf_len,buffer,(size_t)size);
    binbuf_len+=(size_t)size;
    return 1;
  } /* if */
  binbuf_flush((FILE*)handle);
  #if defined _MSC_VER
    return (int)_fwrite_nolock(buffer,1,size,(FILE*)handle) == size;
  #else
    return (int)fwrite(buffer,1,size,(FILE*)handle) == size;
  #endif
}

long pc_lengthbin(void *handle)
{
  binbuf_flush((FILE*)handle);
  return ftell((FILE*)handle);
}
