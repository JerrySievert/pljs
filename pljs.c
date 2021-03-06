#include "postgres.h"
#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif
#include "fmgr.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#if PG_VERSION_NUM >= 90400
#include "utils/jsonb.h"
#endif

#include "pljs.h"


PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljs_call_handler);
PG_FUNCTION_INFO_V1(pljs_call_validator);

Datum pljs_call_handler(PG_FUNCTION_ARGS);
Datum pljs_call_validator(PG_FUNCTION_ARGS);

#if PG_VERSION_NUM >= 90000
//PG_FUNCTION_INFO_V1(pljs_inline_handler);
//Datum pljs_inline_handler(PG_FUNCTION_ARGS);
#endif


static char *dump_error(JSContext *ctx) {
  JSValue exception_val, val;
  const char *stack;
  const char *str;
  bool is_error;
  char *ret = NULL;
  size_t s1, s2;

  exception_val = JS_GetException(ctx);
  is_error = JS_IsError(ctx, exception_val);
  str = JS_ToCStringLen(ctx, &s1, exception_val);

  if (!str) {
    elog(NOTICE, "error thrown but no error message");
    return NULL;
  }

  if (!is_error) {
    ret = (char *) palloc((s1 + 8) * sizeof(char));
    sprintf(ret, "Throw:\n%s", str);
  } else {
    val = JS_GetPropertyStr(ctx, exception_val, "stack");

    if (!JS_IsUndefined(val)) {
        stack = JS_ToCStringLen(ctx, &s2, val);

        ret = (char *) palloc((s1 + s2 + 2) * sizeof(char));
        sprintf(ret, "%s\n%s", str, stack);
        JS_FreeCString(ctx, stack);
    }
    JS_FreeValue(ctx, val);
  }

  JS_FreeCString(ctx, str);
  JS_FreeValue(ctx, exception_val);

  return ret;
}


JSRuntime *rt = NULL;

// hash table for storing caches
HTAB *pljs_HashTable = NULL;

// initialization function
void _PG_init(void) {
  // initialize cache
  HASHCTL ctl = { 0 };

  ctl.keysize = sizeof(pljs_cache_key);
  ctl.entrysize = sizeof(pljs_cache_entry);
  ctl.hash = oid_hash;
  pljs_HashTable = hash_create("pljs function cache",
    32, // arbitrary guess at number of functions per user
    &ctl,
    HASH_ELEM | HASH_FUNCTION);

  rt = JS_NewRuntime();
}

Datum pljs_call_handler(PG_FUNCTION_ARGS)
{
  Oid           fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple     proctuple;
  Form_pg_proc  pg_proc_entry;
  const char   *sourcecode;
  char        **arguments;
  Oid          *argtypes;
  char         *argmodes;
  int           nargs;
  Datum         prosrcdatum;
  bool          isnull;
  JSContext    *ctx;
  Datum         retval;

  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  prosrcdatum = SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);

  if (isnull) {
    elog(ERROR, "null prosrc");
  }

  sourcecode = DatumGetCString(DirectFunctionCall1(textout, prosrcdatum));

  pg_proc_entry = (Form_pg_proc) GETSTRUCT(proctuple);
  nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);
#if 0
  pljs_cache_entry *entry = pljs_get_cache_entry(fn_oid, fcinfo, false, NULL);
  if (entry == NULL) {
    PG_RETURN_VOID();
  }

  ctx = entry->fn->ctx;
  JSValue func = entry->fn->func;
#else
  ctx = JS_NewContext(rt);

  // compile the function into the javascript context
  JSValue func = pljs_compile_function(ctx, NameStr(pg_proc_entry->proname), sourcecode, nargs, arguments);
  if (JS_IsUndefined(func)) {
    JS_FreeContext(ctx);
    PG_RETURN_VOID();
  }
#endif
  // do the logic
  retval = call_function(fcinfo, ctx, func, nargs, argtypes, pg_proc_entry->prorettype);

  // do not destroy the context, it will get reused
  JS_FreeContext(ctx);

  ReleaseSysCache(proctuple);

  return retval;
}

Datum pljs_call_validator(PG_FUNCTION_ARGS)
{
  Oid           fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple     proctuple;
  const char   *sourcecode;
  Datum         prosrcdatum;
  bool          isnull;
  JSContext    *ctx;

  if (fcinfo->flinfo->fn_extra) {
    elog(NOTICE, "fn_extra on validate");
  }
  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  prosrcdatum = SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);

  if (isnull) {
    elog(ERROR, "null prosrc");
  }

  sourcecode = TextDatumGetCString(prosrcdatum);
  //  sourcecode = DatumGetCString(DirectFunctionCall1(textout, prosrcdatum));
  elog(NOTICE, "sourcecode: %s", sourcecode);

  ctx = JS_NewContext(rt);

  elog(NOTICE, "preparing to compile");

  JSValue val = JS_Eval(ctx, sourcecode, strlen(sourcecode), "<function>", JS_EVAL_FLAG_COMPILE_ONLY);

  if (JS_IsException(val)) {
    ereport(ERROR,
      (errmsg("execution error"),
       errdetail("%s", dump_error(ctx))
     ));
  }

  // call validator can release the context
  JS_FreeContext(ctx);

  ReleaseSysCache(proctuple);

  PG_RETURN_VOID();
}


JSValue pljs_compile_function(
  JSContext *ctx,
  char       *name,
  const char *source,
  int nargs,
  char *arguments[])
{
  StringInfoData src;
  int i;

  initStringInfo(&src);

  // generate the function as javascript with all of its arguments
  appendStringInfo(&src, "function %s (", name);

  for (i = 0; i < nargs; i++) {
    // commas between arguments
    if (i > 0) {
      appendStringInfoChar(&src, ',');
    }

    // if this is a named argument, append it
    if (arguments && arguments[i]) {
      appendStringInfoString(&src, arguments[i]);
    } else {
      // otherwise append it as an unnamed argument with a number
      appendStringInfo(&src, "$%d", i + 1);
    }
  }

  appendStringInfo(&src, ") {\n%s\n}\n %s;\n", source, name);

  JSValue val = JS_Eval(ctx, src.data, strlen(src.data), "<function>", 0);

  if (!JS_IsException(val)) {
    pfree(src.data);

    return val;
  } else {
    ereport(ERROR,
      (errmsg("execution error"),
       errdetail("%s", dump_error(ctx))
     ));

    return JS_UNDEFINED;
  }
}

static Datum call_function(FunctionCallInfo fcinfo, JSContext *ctx, JSValue func, int nargs, Oid *argtypes, Oid rettype)
{
  char *str;
#if PG_VERSION_NUM >= 90400
  Jsonb *jb;
#endif

  JSValueConst *argv = (JSValueConst *) palloc(sizeof(JSValueConst) * nargs);

  for (int i = 0; i < nargs; i++) {
    if (fcinfo->argnull[i] == 1) {
      argv[i] = JS_NULL;
      continue;
    }

    switch (argtypes[i]) {
      case OIDOID:
        argv[i] = JS_NewInt64(ctx, fcinfo->arg[i]);
        break;

      case BOOLOID:
        argv[i] = JS_NewBool(ctx, DatumGetBool(fcinfo->arg[i]));
        break;

      case INT2OID:
        argv[i] = JS_NewInt32(ctx, DatumGetInt16(fcinfo->arg[i]));
        break;

      case INT4OID:
        argv[i] = JS_NewInt32(ctx, DatumGetInt32(fcinfo->arg[i]));
        break;

      case INT8OID:
        argv[i] = JS_NewInt64(ctx, DatumGetInt64(fcinfo->arg[i]));
        break;

      case FLOAT4OID:
        argv[i] = JS_NewFloat64(ctx, DatumGetFloat4(fcinfo->arg[i]));
        break;

      case FLOAT8OID:
        argv[i] = JS_NewFloat64(ctx, DatumGetFloat8(fcinfo->arg[i]));
        break;

      case NUMERICOID:
        argv[i] = JS_NewFloat64(ctx, DatumGetFloat8(DirectFunctionCall1(numeric_float8, fcinfo->arg[i])));
        break;

      case TEXTOID:
      case VARCHAROID:
      case BPCHAROID:
      case XMLOID:
        // get a copy of the string
        str = dup_pgtext(PG_GETARG_TEXT_P(i));

        argv[i] = JS_NewString(ctx, str);

        // free the memory allocated
        pfree(str);
        break;
#if PG_VERSION_NUM >= 90200
      case JSONOID:
        // get a copy of the string
        str = dup_pgtext(PG_GETARG_TEXT_P(i));

        argv[i] = JS_ParseJSON(ctx, str, strlen(str), NULL);

        // free the memory allocated
        pfree(str);
        break;

#endif
#if PG_VERSION_NUM >= 90400
        case JSONBOID:
          // get the datum
#if PG_VERSION_NUM < 110000
          jb = PG_GETARG_JSONB(i);
#else
          jb = PG_GETARG_JSONB_P(i);
#endif
          // convert it to a string (takes some casting, but JsonbContainer is also a varlena)
          str = JsonbToCString(NULL, (JsonbContainer *) VARDATA(jb), VARSIZE(jb));

          argv[i] = JS_ParseJSON(ctx, str, strlen(str), NULL);

          // free the memory allocated
          pfree(str);
          break;
#endif

      default:
        elog(NOTICE, "Unknown type: %d", argtypes[i]);
        argv[i] = JS_NULL;
    }
  }

  JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, nargs, argv);
  if (JS_IsException(ret)) {
    ereport(ERROR,
      (errmsg("execution error"),
       errdetail("%s", dump_error(ctx))
     ));

    JS_FreeValue(ctx, ret);

    PG_RETURN_VOID();
  } else {
    Datum d = ctx_to_datum(fcinfo, ctx, ret, rettype);
    JS_FreeValue(ctx, ret);

    return d;
  }
}

// allocate memory and copy data from the varlena text representation
static char *dup_pgtext(text *what)
{
  size_t len = VARSIZE(what) - VARHDRSZ;
  char  *dup = palloc(len + 1);

  memcpy(dup, VARDATA(what), len);
  dup[len] = 0;

  return dup;
}

static Datum ctx_to_datum(FunctionCallInfo fcinfo, JSContext *ctx, JSValue val, Oid rettype)
{
  switch (rettype) {
    case VOIDOID:
      PG_RETURN_VOID();
      break;

    case OIDOID: {
      int64_t in;
      JS_ToInt64(ctx, &in, val);

      PG_RETURN_OID(in);
      break;
    }

    case BOOLOID: {
      int8_t in = JS_ToBool(ctx, val);
      PG_RETURN_BOOL(in);
      break;
    }

    case INT2OID: {
      int32_t in;
      JS_ToInt32(ctx, &in, val);

      PG_RETURN_INT16((int16_t) in);
      break;
    }

    case INT4OID: {
      int32_t in;
      JS_ToInt32(ctx, &in, val);
      PG_RETURN_INT32(in);
      break;
    }

    case INT8OID: {
      int64_t in;
      JS_ToInt64(ctx, &in, val);

      PG_RETURN_INT64(in);
      break;
    }

    case FLOAT4OID: {
      double in;
      JS_ToFloat64(ctx, &in, val);

      PG_RETURN_FLOAT4((float4) in);
      break;
    }

    case FLOAT8OID: {
      double in;
      JS_ToFloat64(ctx, &in, val);

      PG_RETURN_FLOAT8(in);
      break;
    }

    case NUMERICOID: {
      double in;
      JS_ToFloat64(ctx, &in, val);

      return DirectFunctionCall1(float8_numeric, Float8GetDatum((float8) in));
      break;
    }


    case TEXTOID:
    case VARCHAROID:
    case BPCHAROID:
    case XMLOID: {
      size_t plen;
      const char *str = JS_ToCStringLen(ctx, &plen, val);

      text *t = (text *) palloc(plen + VARHDRSZ);
      SET_VARSIZE(t, plen + VARHDRSZ);
      memcpy(VARDATA(t), str, plen);
      JS_FreeCString(ctx, str);

      PG_RETURN_TEXT_P(t);
      break;
    }

#if PG_VERSION_NUM >= 90200
    case JSONOID: {
      JSValue obj = JS_NewObject(ctx);
      JSValue js = js_json_stringify(ctx, obj, 1, &val);
      size_t plen;
      const char *str = JS_ToCStringLen(ctx, &plen, js);

      text *t = (text *) palloc(plen + VARHDRSZ);
      SET_VARSIZE(t, plen + VARHDRSZ);
      memcpy(VARDATA(t), str, plen);
      JS_FreeCString(ctx, str);
      JS_FreeValue(ctx, obj);

      // return it as a CStringTextDatum
      return CStringGetTextDatum(str);
      break;
    }
#endif

#if PG_VERSION_NUM >= 90400
    case JSONBOID: {
      JSValue obj = JS_NewObject(ctx);
      JSValue js = js_json_stringify(ctx, obj, 1, &val);
      size_t plen;
      const char *str = JS_ToCStringLen(ctx, &plen, js);

      text *t = (text *) palloc(plen + VARHDRSZ);
      SET_VARSIZE(t, plen + VARHDRSZ);
      memcpy(VARDATA(t), str, plen);
      JS_FreeCString(ctx, str);
      JS_FreeValue(ctx, obj);

      // return it as a Datum, since there is no direct CStringGetJsonb exposed
#if PG_VERSION_NUM < 110000
      return (Datum) DatumGetJsonb(DirectFunctionCall1(jsonb_in, (Datum) (char *) str));
#else
      return (Datum) DatumGetJsonbP(DirectFunctionCall1(jsonb_in, (Datum) (char *) str));
#endif
      break;
    }
#endif

    default:
      elog(NOTICE, "Unknown type: %d", rettype);
      PG_RETURN_NULL();
  }

  // shut up compiler
  PG_RETURN_VOID();
}
