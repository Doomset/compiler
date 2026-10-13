/*  Pawn compiler  - maintenance of various lists
 *
 *  o  Name list (aliases)
 *  o  Include path list
 *  o  Macro definitions (text substitutions)
 *  o  Documentation tags and automatic listings
 *  o  Debug strings
 *
 *  Copyright (c) ITB CompuPhase, 2001-2006
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
 *  Version: $Id: sclist.c 3660 2006-11-05 13:05:09Z thiadmer $
 */

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "sc.h"

#if defined FORTIFY
  #include <alloc/fortify.h>
#endif

/* a "private" implementation of strdup(), so that porting
 * to other memory allocators becomes easier.
 * By S�ren Hannibal.
 */
SC_FUNC char* duplicatestring(const char* sourcestring)
{
  char* result=(char*)malloc(strlen(sourcestring)+1);
  if (result!=NULL)
    strcpy(result,sourcestring);
  return result;
}


static stringpair *insert_stringpair_from(stringpair *root,stringpair *start,char *first,char *second,int matchlength)
{
  stringpair *cur,*pred;

  assert(root!=NULL);
  assert(first!=NULL);
  assert(second!=NULL);
  /* create a new node, and check whether all is okay */
  if ((cur=(stringpair*)malloc(sizeof(stringpair)))==NULL)
    return NULL;
  cur->first=duplicatestring(first);
  cur->second=duplicatestring(second);
  cur->matchlength=matchlength;
  if (cur->first==NULL || cur->second==NULL) {
    free(cur->first);
    free(cur->second);
    free(cur);
    return NULL;
  } /* if */
  /* link the node to the tree, find the position */
  /* "start" is root, or a node of the list whose string is smaller than "first" */
  for (pred=(start!=NULL) ? start : root; pred->next!=NULL && strcmp(pred->next->first,first)<0; pred=pred->next)
    /* nothing */;
  cur->next=pred->next;
  pred->next=cur;
  return cur;
}

static stringpair *insert_stringpair(stringpair *root,char *first,char *second,int matchlength)
{
  return insert_stringpair_from(root,NULL,first,second,matchlength);
}

static void delete_stringpairtable(stringpair *root)
{
  stringpair *cur, *next;

  assert(root!=NULL);
  cur=root->next;
  while (cur!=NULL) {
    next=cur->next;
    assert(cur->first!=NULL);
    assert(cur->second!=NULL);
    free(cur->first);
    free(cur->second);
    free(cur);
    cur=next;
  } /* while */
  memset(root,0,sizeof(stringpair));
}

static stringpair *find_stringpair(stringpair *cur,char *first,int matchlength)
{
  int result=0;

  assert(matchlength>0);  /* the function cannot handle zero-length comparison */
  assert(first!=NULL);
  while (cur!=NULL && result<=0) {
    result=(int)*cur->first - (int)*first;
    if (result==0 && matchlength==cur->matchlength) {
      result=strncmp(cur->first,first,matchlength);
      if (result==0)
        return cur;
    } /* if */
    cur=cur->next;
  } /* while */
  return NULL;
}

/* "start" is root, or a node that comes before "item" in the list */
static int delete_stringpair_from(stringpair *root,stringpair *start,stringpair *item)
{
  stringpair *cur;

  assert(root!=NULL);
  cur=(start!=NULL) ? start : root;
  while (cur->next!=NULL) {
    if (cur->next==item) {
      cur->next=item->next;     /* unlink from list */
      assert(item->first!=NULL);
      assert(item->second!=NULL);
      free(item->first);
      free(item->second);
      free(item);
      return TRUE;
    } /* if */
    cur=cur->next;
  } /* while */
  return FALSE;
}

static int delete_stringpair(stringpair *root,stringpair *item)
{
  return delete_stringpair_from(root,NULL,item);
}

/* ----- string list functions ----------------------------------- */
static stringlist *insert_string(stringlist *list,char *string)
{
  stringlist newlist;

  assert(list!=NULL);
  assert(string!=NULL);
  if (list->data==NULL) {
    /* inserted for the first time */
    newlist.length=0;
    newlist.size=2;
    newlist.data=calloc(newlist.size,sizeof(char*));
    newlist.strings=newlist.data;
    if (newlist.data==NULL)
      error(103);       /* insufficient memory (fatal error) */
    *list=newlist;
  } else if (list->length==list->size ||
             list->length==list->data+list->size-list->strings) {
    /* allocate more elements */
    newlist.length=list->length;
    newlist.size=list->size+list->size/2;
    newlist.data=realloc(list->data,newlist.size*sizeof(char*));
    newlist.strings=newlist.data+(list->strings-list->data);
    if (newlist.data==NULL)
      error(103);       /* insufficient memory (fatal error) */
    memset(newlist.data+list->size,0,list->size/2*sizeof(char*));
    *list=newlist;
  } /* if */
  if ((list->strings[list->length]=duplicatestring(string))==NULL)
    error(103);         /* insufficient memory (fatal error) */
  list->length++;
  return list;
}

static char *get_string(stringlist *list,int index)
{
  assert(list!=NULL);
  if (index>=0 && index<list->length)
    return list->strings[index];
  return NULL;
}

static int delete_string(stringlist *list,int index)
{
  assert(list!=NULL);
  assert(index==0);
  if (list->length>0) {
    free(list->strings[0]);
    list->strings[0]=NULL;
    list->strings++;
    list->length--;
    return TRUE;
  } /* if */
  return FALSE;
}

SC_FUNC void delete_stringtable(stringlist *list)
{
  int i;

  assert(list!=NULL);
  for (i=0; i<list->size; i++)
    free(list->data[i]);
  free(list->data);
  memset(list,0,sizeof(stringlist));
}


/* ----- alias table --------------------------------------------- */
static stringpair alias_tab = {NULL, NULL, NULL};   /* alias table */

SC_FUNC stringpair *insert_alias(char *name,char *alias)
{
  stringpair *cur;

  assert(name!=NULL);
  assert(strlen(name)<=sNAMEMAX);
  assert(alias!=NULL);
  assert(strlen(alias)<=sNAMEMAX);
  if ((cur=insert_stringpair(&alias_tab,name,alias,strlen(name)))==NULL)
    error(103);       /* insufficient memory (fatal error) */
  return cur;
}

SC_FUNC int lookup_alias(char *target,char *name)
{
  stringpair *cur=find_stringpair(alias_tab.next,name,strlen(name));
  if (cur!=NULL) {
    assert(strlen(cur->second)<=sNAMEMAX);
    strcpy(target,cur->second);
  } /* if */
  return cur!=NULL;
}

SC_FUNC void delete_aliastable(void)
{
  delete_stringpairtable(&alias_tab);
}

/* ----- include paths list -------------------------------------- */
static stringlist includepaths = {NULL, NULL, 0, 0};  /* directory list for include files */

SC_FUNC stringlist *insert_path(char *path)
{
  return insert_string(&includepaths,path);
}

SC_FUNC char *get_path(int index)
{
  return get_string(&includepaths,index);
}

SC_FUNC void delete_pathtable(void)
{
  delete_stringtable(&includepaths);
  assert(includepaths.strings==NULL);
}


/* ----- text substitution patterns ------------------------------ */
#if !defined NO_DEFINE

static stringpair substpair = { NULL, NULL, NULL};  /* list of substitution pairs */

static stringpair *substindex['z'-PUBLIC_CHAR+1]; /* quick index to first character */

/* The list is sorted by string, so all strings that start with one character are together and the
 * buckets follow each other in character order. A search for anything that starts with "c" can therefore
 * begin at the head of the nearest lower bucket instead of at the head of the list: with the 20,000+
 * macros that YSI and the open.mp includes define, every #define / #undef used to walk the whole list
 * (twice). The list that results is the same.
 */
static stringpair *subst_before(char c)
{
  int k;
  for (k=(int)c-PUBLIC_CHAR-1; k>=0; k--)
    if (substindex[k]!=NULL)
      return substindex[k];     /* every string up to and including its head is smaller than one with "c" */
  return NULL;                  /* none: begin at the root */
}

static void adjustindex(char c)
{
  stringpair *cur;
  assert(c>='A' && c<='Z' || c>='a' && c<='z' || c=='_' || c==PUBLIC_CHAR);
  assert(PUBLIC_CHAR<'A' && 'A'<'_' && '_'<'z');

  cur=subst_before(c);
  cur=(cur!=NULL) ? cur : substpair.next;
  for ( ; cur!=NULL && (unsigned char)cur->first[0]<(unsigned char)c; cur=cur->next)
    /* nothing */;
  if (cur!=NULL && cur->first[0]!=c)
    cur=NULL;                   /* sorted: nothing starts with "c" */
  substindex[(int)c-PUBLIC_CHAR]=cur;
}

/* Macro prefixes are unique: "#define" of a prefix that exists deletes the old macro first. So a hash table
 * from (prefix, length) to the list node answers find_subst() with one probe, where the list search walked
 * every macro with the same first character (most identifiers are not macros at all, and YSI has 20,000+
 * macros). The sorted list stays as it was, for insertion and deletion. If the table cannot be allocated,
 * find_subst() falls back to the list search for the rest of the run.
 */
typedef struct { unsigned long long key; stringpair *item; } SUBSTSLOT;
static SUBSTSLOT *subst_map=NULL;
static size_t subst_map_cap=0,subst_map_cnt=0;
static int subst_map_off=FALSE;

static unsigned long long subst_key(const char *s,int len)
{
  unsigned long long h=14695981039346656037ULL;
  int i;
  for (i=0; i<len; i++) {
    h^=(unsigned char)s[i];
    h*=1099511628211ULL;
  } /* for */
  h^=(unsigned long long)len;
  h*=1099511628211ULL;
  return h|1ULL;                /* 0 marks an empty slot */
}

#define SUBST_HOME(key,cap)   ((size_t)((key)>>7)&((cap)-1))

static void subst_map_drop(void)
{
  free(subst_map);
  subst_map=NULL;
  subst_map_cap=subst_map_cnt=0;
}

static void subst_map_put(stringpair *item)
{
  unsigned long long key;
  size_t i;
  if (subst_map_off)
    return;
  if ((subst_map_cnt+1)*2>subst_map_cap) {
    size_t newcap=(subst_map_cap==0) ? 8192 : subst_map_cap*2;
    SUBSTSLOT *newmap=(SUBSTSLOT *)calloc(newcap,sizeof(SUBSTSLOT));
    size_t k;
    if (newmap==NULL) {
      subst_map_drop();
      subst_map_off=TRUE;       /* find_subst() searches the list from now on */
      return;
    } /* if */
    for (k=0; k<subst_map_cap; k++) {
      if (subst_map[k].key!=0) {
        for (i=SUBST_HOME(subst_map[k].key,newcap); newmap[i].key!=0; i=(i+1)&(newcap-1))
          /* nothing */;
        newmap[i]=subst_map[k];
      } /* if */
    } /* for */
    free(subst_map);
    subst_map=newmap;
    subst_map_cap=newcap;
  } /* if */
  key=subst_key(item->first,item->matchlength);
  for (i=SUBST_HOME(key,subst_map_cap); subst_map[i].key!=0; i=(i+1)&(subst_map_cap-1))
    /* nothing */;
  subst_map[i].key=key;
  subst_map[i].item=item;
  subst_map_cnt++;
}

static stringpair *subst_map_get(const char *name,int length)
{
  unsigned long long key=subst_key(name,length);
  size_t i;
  for (i=SUBST_HOME(key,subst_map_cap); subst_map[i].key!=0; i=(i+1)&(subst_map_cap-1)) {
    stringpair *item=subst_map[i].item;
    if (subst_map[i].key==key && item->matchlength==length && strncmp(item->first,name,length)==0)
      return item;
  } /* for */
  return NULL;
}

static void subst_map_remove(stringpair *item)
{
  unsigned long long key;
  size_t i,j,k,mask;
  if (subst_map==NULL)
    return;
  mask=subst_map_cap-1;
  key=subst_key(item->first,item->matchlength);
  for (i=SUBST_HOME(key,subst_map_cap); subst_map[i].key!=0 && subst_map[i].item!=item; i=(i+1)&mask)
    /* nothing */;
  if (subst_map[i].key==0)
    return;
  /* linear probing: delete by shifting later entries of the cluster back */
  subst_map[i].key=0;
  subst_map[i].item=NULL;
  subst_map_cnt--;
  for (j=(i+1)&mask; subst_map[j].key!=0; j=(j+1)&mask) {
    k=SUBST_HOME(subst_map[j].key,subst_map_cap);
    if ((i<=j) ? (i<k && k<=j) : (i<k || k<=j))
      continue;                 /* its home lies between the hole and itself: it stays */
    subst_map[i]=subst_map[j];
    subst_map[j].key=0;
    subst_map[j].item=NULL;
    i=j;
  } /* for */
}

static stringpair *subst_list_find(char *name,int length)
{
  stringpair *item=substindex[(int)*name-PUBLIC_CHAR];
  if (item!=NULL)
    item=find_stringpair(item,name,length);
  return item;
}

SC_FUNC stringpair *insert_subst(char *pattern,char *substitution,int prefixlen)
{
  stringpair *cur;

  assert(pattern!=NULL);
  assert(substitution!=NULL);
  if ((cur=insert_stringpair_from(&substpair,subst_before(*pattern),pattern,substitution,prefixlen))==NULL)
    error(103);       /* insufficient memory (fatal error) */
  subst_map_put(cur);
  adjustindex(*pattern);
  return cur;
}

SC_FUNC stringpair *find_subst(char *name,int length)
{
  assert(name!=NULL);
  assert(length>0);
  assert(*name>='A' && *name<='Z' || *name>='a' && *name<='z' || *name=='_' || *name==PUBLIC_CHAR);
  if (subst_map_off)
    return subst_list_find(name,length);
  if (subst_map==NULL)
    return NULL;                /* no macros */
  return subst_map_get(name,length);
}

SC_FUNC int delete_subst(char *name,int length)
{
  stringpair *item;
  assert(name!=NULL);
  assert(length>0);
  assert(*name>='A' && *name<='Z' || *name>='a' && *name<='z' || *name=='_' || *name==PUBLIC_CHAR);
  item=find_subst(name,length);
  if (item==NULL)
    return FALSE;
  subst_map_remove(item);
  delete_stringpair_from(&substpair,subst_before(*name),item);
  adjustindex(*name);
  return TRUE;
}

SC_FUNC void delete_substtable(void)
{
  int i;
  delete_stringpairtable(&substpair);
  for (i=0; i<arraysize(substindex); i++)
    substindex[i]=NULL;
  subst_map_drop();
  subst_map_off=FALSE;
}

#endif /* !defined NO_SUBST */


/* ----- input file list ----------------------------------------- */
static stringlist sourcefiles = {NULL, NULL, 0, 0};

SC_FUNC stringlist *insert_sourcefile(char *string)
{
  return insert_string(&sourcefiles,string);
}

SC_FUNC char *get_sourcefile(int index)
{
  return get_string(&sourcefiles,index);
}

SC_FUNC void delete_sourcefiletable(void)
{
  delete_stringtable(&sourcefiles);
  assert(sourcefiles.strings==NULL);
}


/* ----- documentation tags -------------------------------------- */
#if !defined SC_LIGHT
static stringlist docstrings = {NULL, NULL, 0, 0};

SC_FUNC stringlist *insert_docstring(char *string)
{
  return insert_string(&docstrings,string);
}

SC_FUNC char *get_docstring(int index)
{
  return get_string(&docstrings,index);
}

SC_FUNC void delete_docstring(int index)
{
  assert(index==0);
  delete_string(&docstrings, index);
}

SC_FUNC void delete_docstringtable(void)
{
  delete_stringtable(&docstrings);
  assert(docstrings.strings==NULL);
}
#endif /* !defined SC_LIGHT */


/* ----- autolisting --------------------------------------------- */
static stringlist autolist = {NULL, NULL, 0, 0};

SC_FUNC stringlist *insert_autolist(char *string)
{
  return insert_string(&autolist,string);
}

SC_FUNC char *get_autolist(int index)
{
  return get_string(&autolist,index);
}

SC_FUNC void delete_autolisttable(void)
{
  delete_stringtable(&autolist);
  assert(autolist.strings==NULL);
}


/* ----- value pair list ----------------------------------------- */
static valuepair heaplist = {NULL, 0, 0};

SC_FUNC valuepair *push_heaplist(long first, long second)
{
  valuepair *cur, *last;
  if ((cur=malloc(sizeof(valuepair)))==NULL)
    error(103);       /* insufficient memory (fatal error) */

  cur->first=first;
  cur->second=second;
  cur->next=NULL;

  for (last=&heaplist; last->next!=NULL; last=last->next)
    /* nothing */;
  last->next=cur;
  return cur;
}

SC_FUNC int popfront_heaplist(long *first, long *second)
{
  valuepair *front=heaplist.next;
  if (front==NULL)
    return 0;

  /* copy fields */
  *first=front->first;
  *second=front->second;

  /* unlink and free */
  heaplist.next=front->next;
  free(front);
  return 1;
}

SC_FUNC valuepair *heaplist_first(void)
{
  return heaplist.next;
}

SC_FUNC void delete_heaplisttable(void)
{
  valuepair *cur;
  while (heaplist.next!=NULL) {
    cur=heaplist.next;
    heaplist.next=cur->next;
    free(cur);
  } /* while */
}


/* ----- debug information --------------------------------------- */
static stringlist dbgstrings = {NULL, NULL, 0, 0};

SC_FUNC stringlist *insert_dbgfile(const char *filename)
{
  if (sc_status==statWRITE && (sc_debug & sSYMBOLIC)!=0) {
    char string[_MAX_PATH+40];
    assert(filename!=NULL);
    assert(strlen(filename)+40<arraysize(string));
    sprintf(string,"F:%" PRIxC " %s",code_idx,filename);
    return insert_string(&dbgstrings,string);
  } /* if */
  return NULL;
}

SC_FUNC stringlist *insert_dbgline(int linenr)
{
  if (sc_status==statWRITE && (sc_debug & sSYMBOLIC)!=0) {
    char string[40];
    if (linenr>0)
      linenr--;         /* line numbers are zero-based in the debug information */
    sprintf(string,"L:%" PRIxC " %x",code_idx,linenr);
    return insert_string(&dbgstrings,string);
  } /* if */
  return NULL;
}

SC_FUNC stringlist *insert_dbgsymbol(symbol *sym)
{
  if (sc_status==statWRITE && (sc_debug & sSYMBOLIC)!=0) {
    char string[2*sNAMEMAX+128];
    char symname[2*sNAMEMAX+16];

    funcdisplayname(symname,sym->name);
    /* address tag:name codestart codeend ident vclass [tag:dim ...] */
    if (sym->ident==iFUNCTN) {
      sprintf(string,"S:%" PRIxC " %x:%s %" PRIxC " %" PRIxC " %x %x",
              sym->addr,sym->tag,symname,sym->addr,sym->codeaddr,sym->ident,sym->vclass);
    } else {
      sprintf(string,"S:%" PRIxC " %x:%s %" PRIxC " %" PRIxC " %x %x",
              sym->addr,sym->tag,symname,sym->codeaddr,code_idx,sym->ident,sym->vclass);
    } /* if */
    if (sym->ident==iARRAY || sym->ident==iREFARRAY) {
      #if !defined NDEBUG
        int count=sym->dim.array.level;
      #endif
      symbol *sub;
      strcat(string," [ ");
      for (sub=sym; sub!=NULL; sub=sub->child) {
        assert(sub->dim.array.level==count--);
        sprintf(string+strlen(string),"%x:%x ",sub->x.tags.index,sub->dim.array.length);
      } /* for */
      strcat(string,"]");
    } /* if */

    return insert_string(&dbgstrings,string);
  } /* if */
  return NULL;
}

SC_FUNC char *get_dbgstring(int index)
{
  return get_string(&dbgstrings,index);
}

SC_FUNC void delete_dbgstringtable(void)
{
  delete_stringtable(&dbgstrings);
  assert(dbgstrings.strings==NULL);
}
