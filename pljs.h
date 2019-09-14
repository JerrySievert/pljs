#ifndef PLJS_H
#define PLJS_H

#include "deps/quickjs/quickjs-libc.h"

// static functions in pljs.c
static Datum call_function(PG_FUNCTION_ARGS, JSContext *, JSValue, int, Oid *, Oid);
static char *dup_pgtext(text *);
static Datum ctx_to_datum(FunctionCallInfo, JSContext *, JSValue, Oid);

JSValue pljs_compile_function(JSContext *, char *, const char *, int, char *[]);

// other functions
void _PG_init(void);

// cache key
typedef struct pljs_cache_key {
  Oid  fn_oid;
  bool trigger;
  Oid  user_id;
  int  nargs;
  Oid  argtypes[FUNC_MAX_ARGS];
} pljs_cache_key;


// function definition
typedef struct pljs_function {
  Oid             fn_oid;
  char            proname[NAMEDATALEN];
  char           *prosrc;
  int             nargs;
  Oid             rettype;
  Oid             argtypes[FUNC_MAX_ARGS];
  JSValue         func;
  JSContext      *ctx;
  pljs_cache_key *key;
} pljs_function;

// cache entry containing the function
typedef struct pljs_cache_entry {
  pljs_function  *fn;
  pljs_cache_key  key;
} pljs_cache_entry;


extern JSRuntime *rt;
extern HTAB *pljs_HashTable;

pljs_cache_entry *pljs_get_cache_entry(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, char ***argnames);
pljs_cache_entry *pljs_hash_table_search(Oid, bool, Oid, int, Oid *);
void pljs_hash_table_insert(pljs_function *, pljs_cache_key *);

#endif // PLJS_H
