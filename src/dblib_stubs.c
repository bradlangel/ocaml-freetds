/* File: dblib_stubs.c

   Copyright (C) 2010

     Christophe Troestler <Christophe.Troestler@umons.ac.be>
     WWW: http://math.umons.ac.be/an/software/

   This library is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License version 3 or
   later as published by the Free Software Foundation.  See the file
   LICENCE for more details.

   This library is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the file
   LICENSE for more details. */

/* Binding to the DB-Library part of freetds.
   See http://www.freetds.org/userguide/samplecode.htm */

#include <sybfront.h> /* sqlfront.h always comes first */
#include <sybdb.h>
#include <syberror.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <caml/mlvalues.h>
#include <caml/alloc.h>
#include <caml/memory.h>
#include <caml/fail.h>
#include <caml/callback.h>
#include <caml/custom.h>

static void raise_fatal(char *msg)
{
  CAMLparam0();
  CAMLlocal1(vmsg);
  value args[2];
  static value *exn = NULL;
  if (exn == NULL) {
    /* First time around, look up by name */
    exn = caml_named_value("Freetds.Dblib.Error");
  }

  args[0] = Val_int(9); /* FATAL (on OCaml side) */
  vmsg = caml_copy_string(msg);
  args[1] = vmsg;
  caml_raise_with_args(*exn, 2, args);
  CAMLreturn0;
}

static int convert_severity(int severity)
{
  /* Keep in sync with dblib.ml */
  severity -= EXINFO;
  if (severity >= 11) /* number of constructors on the OCaml side */
    severity = 9;
  return severity;
}

static int msg_handler(DBPROCESS *dbproc, DBINT msgno, int msgstate, int severity,
		       char *msgtext, char *srvname, char *procname, int line)
{
  enum {changed_database = 5701, changed_language = 5703 };

  if (msgno == changed_database || msgno == changed_language)
    return 0;

  CAMLparam0();
  CAMLlocal3(vseverity, vline, vmsg);
  static value *handler = NULL;

  if (handler == NULL) {
    /* First time around, look up by name */
    handler = caml_named_value("Freetds.Dblib.msg_handler");
  }

  severity = convert_severity(severity);
  vseverity = Val_int(severity);
  vline = Val_int(line);
  vmsg = caml_copy_string(msgtext);

  caml_callback3(*handler, vseverity, vline, vmsg);

  CAMLreturn(INT_CANCEL); /* should not return */
}

/* http://manuals.sybase.com/onlinebooks/group-cnarc/cng1110e/dblib/@Generic__BookTextView/16561;pt=39614 */
static int err_handler(DBPROCESS *dbproc, int severity, /* in syberror.h */
                       int dberr, /* in sybdb.h, SYBE* macros */
                       int oserr, /* in sybdb.h */
                       char *dberrstr, char *oserrstr)
{
  CAMLparam0();
  CAMLlocal1(vmsg);
  static value *handler = NULL;
  value args[2];
  static value *exn = NULL;

  if (exn == NULL) {
    /* First time around, look up by name */
    exn = caml_named_value("Freetds.Dblib.Error");
  }
  severity = convert_severity(severity);
  args[0] = Val_int(severity);

#define RAISE(msg)                              \
  vmsg = caml_copy_string(msg);                 \
  args[1] = vmsg;                               \
  caml_raise_with_args(*exn, 2, args);

  if ((dbproc == NULL) || (DBDEAD(dbproc))) {
    /* Always raise an exception when the DB handle is unusable. */
    RAISE(dberrstr);
  }
  if (oserr != DBNOERR && oserr != 0 /* Undefined error */) {
    RAISE(oserrstr);
  }
  if (dberr == SYBESMSG) {
    RAISE("Check messages from the SQL Server");
  }

  if (handler == NULL) {
    /* First time around, look up by name */
    handler = caml_named_value("Freetds.Dblib.err_handler");
  }
  vmsg = caml_copy_string(dberrstr);
  caml_callback3(*handler, args[0] /* severity */, Val_int(dberr), vmsg);
  /* FIXME: allow the handler to return whether to continue trying on
     timeouts?  Separate handler for timeouts?? */

  CAMLreturn(INT_CANCEL); /* should not return */
}


CAMLexport value ocaml_freetds_dbinit(value unit)
{
  CAMLparam0();
  if (dbinit() == FAIL) {
    raise_fatal("Cannot initialize DB-lib!");
  }
  /* Install a error handler using exceptions, the default error
   * handler aborts the program under some circumstances. */
  dberrhandle(&err_handler);
  dbmsghandle(&msg_handler);
  CAMLreturn(Val_unit);
}

#define DBPROCESS_VAL(v) (* (DBPROCESS **) Data_custom_val(v))
#define DBPROCESS_ALLOC()                                       \
  alloc_custom(&dbprocess_ops, sizeof(DBPROCESS *), 1, 30)

static int dbprocess_compare(value v1, value v2)
{
  /* Compare pointers */
  if (DBPROCESS_VAL(v1) < DBPROCESS_VAL(v2)) return(-1);
  else if (DBPROCESS_VAL(v1) > DBPROCESS_VAL(v2)) return(1);
  else return(0);
}

static long dbprocess_hash(value v)
{
  /* The pointer will do a good hash and respect compare v1 v2 = 0 ==>
     hash(v1) = hash(v2) */
  return((long) DBPROCESS_VAL(v));
}

static struct custom_operations dbprocess_ops = {
  "freetds/dbprocess", /* identifier for serialization and deserialization */
  custom_finalize_default, /* one must call dbclose */
  &dbprocess_compare,
  &dbprocess_hash,
  custom_serialize_default,
  custom_deserialize_default
};


CAMLexport
value ocaml_freetds_dbopen(value vuser, value vpasswd, value vchar_set,
                           value vlanguage, value vapplication,
                           value vserver)
{
  CAMLparam5(vuser, vpasswd, vapplication, vchar_set, vlanguage);
  CAMLxparam1(vserver);
  CAMLlocal1(vdbproc);
  LOGINREC *login;
  DBPROCESS *dbproc;

  if ((login = dblogin()) == NULL) {
    raise_fatal("Freetds.Dblib.connect: cannot allocate the login structure");
  }
  if (Is_block(vuser)) /* <> None */
    DBSETLUSER(login, String_val(Field(vuser, 0)));
  if (Is_block(vpasswd)) /* <> None */
    DBSETLPWD(login, String_val(Field(vpasswd, 0)));
  if (Is_block(vchar_set))
    DBSETLCHARSET(login, String_val(Field(vchar_set, 0)));
  if (Is_block(vlanguage))
    DBSETLNATLANG(login, String_val(Field(vlanguage, 0)));
  if (Is_block(vapplication)) /* <> None */
    DBSETLAPP(login, String_val(Field(vapplication, 0)));

  dbproc = dbopen(login, String_val(vserver));
  dbloginfree(login); /* dbopen made => [login] no longer needed. */
  if (dbproc == NULL) {
    raise_fatal("Freetds.Dblib.connect: unable to connect to the database");
  }
  vdbproc = DBPROCESS_ALLOC();
  DBPROCESS_VAL(vdbproc) = dbproc;
  CAMLreturn(vdbproc);
}

CAMLexport value ocaml_freetds_dbopen_bc(value * argv, int argn)
{
  return ocaml_freetds_dbopen(
    argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);
}


CAMLexport value ocaml_freetds_dbclose(value vdbproc)
{
  CAMLparam1(vdbproc);
  dbclose(DBPROCESS_VAL(vdbproc));
  CAMLreturn(Val_unit);
}

CAMLexport value ocaml_freetds_dbuse(value vdbproc, value vdbname)
{
  CAMLparam2(vdbproc, vdbname);
  if (dbuse(DBPROCESS_VAL(vdbproc), String_val(vdbname)) == FAIL) {
    raise_fatal("Freetds.Dblib.use: unable to use the given database");
  }
  CAMLreturn(Val_unit);
}

CAMLexport value ocaml_freetds_dbname(value vdbproc)
{
  CAMLparam1(vdbproc);
  CAMLlocal1(vname);
  char *name;

  name = dbname(DBPROCESS_VAL(vdbproc));
  vname = caml_copy_string(name);
  /* free(name); */ /* generate a segfault */
  CAMLreturn(vname);
}


/* Executing SQL queries
**********************************************************************/

CAMLexport value ocaml_freetds_dbsqlexec(value vdbproc, value vsql)
{
  CAMLparam2(vdbproc, vsql);

  if (dbcmd(DBPROCESS_VAL(vdbproc), String_val(vsql)) == FAIL) {
    raise_fatal("Freetds.Dblib.sqlexec: cannot allocate memory to hold "
                "the SQL query");
  }
  /* Sending the query to the server resets the command buffer. */
  if (dbsqlexec(DBPROCESS_VAL(vdbproc)) == FAIL) {
    raise_fatal("Freetds.Dblib.sqlexec: the SQL query is invalid. It may be "
                "due to a SQL syntax error, incorrect column or table names, "
                "or if the previous query results were not completely "
                "read,...");
  }
  CAMLreturn(Val_unit);
}

CAMLexport value ocaml_freetds_dbresults(value vdbproc)
{
  CAMLparam1(vdbproc);
  RETCODE erc;
  if ((erc = dbresults(DBPROCESS_VAL(vdbproc))) == FAIL) {
    raise_fatal("Freetds.Dblib.results: query was not processed "
                "successfully by the server");
  }
  CAMLreturn(Val_bool(erc == SUCCEED));
}

CAMLexport value ocaml_freetds_numcols(value vdbproc)
{
  /* noalloc */
  return(Val_int(dbnumcols(DBPROCESS_VAL(vdbproc))));
}

CAMLexport value ocaml_freetds_dbcolname(value vdbproc, value vc)
{
  CAMLparam2(vdbproc, vc);
  CAMLlocal1(vname);
  char *name;
  name = dbcolname(DBPROCESS_VAL(vdbproc), Int_val(vc));
  if (name == NULL)
    invalid_argument("FreeTDS.Dblib.colname: column number out of range");
  vname = caml_copy_string(name);
  /* free(name); */ /* Doing it says "invalid pointer". */
  CAMLreturn(vname);
}

CAMLexport value ocaml_freetds_dbcoltype(value vdbproc, value vc)
{
  CAMLparam2(vdbproc, vc);
  /* Keep in sync with "type col_type" on the Caml side. */
  switch (dbcoltype(DBPROCESS_VAL(vdbproc), Int_val(vc))) {
  case SYBCHAR:    CAMLreturn(Val_int(0));
  case SYBVARCHAR: CAMLreturn(Val_int(1));
  case SYBINTN: CAMLreturn(Val_int(2));
  case SYBINT1: CAMLreturn(Val_int(3));
  case SYBINT2: CAMLreturn(Val_int(4));
  case SYBINT4: CAMLreturn(Val_int(5));
  case SYBINT8: CAMLreturn(Val_int(6));
  case SYBFLT8: CAMLreturn(Val_int(7));
  case SYBFLTN: CAMLreturn(Val_int(8));
  case SYBNUMERIC: CAMLreturn(Val_int(9));
  case SYBDECIMAL: CAMLreturn(Val_int(10));
  case SYBDATETIME: CAMLreturn(Val_int(11));
  case SYBDATETIME4: CAMLreturn(Val_int(12));
  case SYBDATETIMN: CAMLreturn(Val_int(13));
  case SYBBIT: CAMLreturn(Val_int(14));
  case SYBTEXT: CAMLreturn(Val_int(15));
  case SYBIMAGE: CAMLreturn(Val_int(16));
  case SYBMONEY4: CAMLreturn(Val_int(17));
  case SYBMONEY: CAMLreturn(Val_int(18));
  case SYBMONEYN: CAMLreturn(Val_int(19));
  case SYBREAL: CAMLreturn(Val_int(20));
  case SYBBINARY: CAMLreturn(Val_int(21));
  case SYBVARBINARY: CAMLreturn(Val_int(22));
  }
  raise_fatal("Freetds.Dblib.coltype: unknown column type"
              " (contact the author of the OCaml bindings)");
}

CAMLexport value ocaml_freetds_dbcancel(value vdbproc)
{
  CAMLparam1(vdbproc);
  dbcancel(DBPROCESS_VAL(vdbproc));
  CAMLreturn(Val_unit);
}

CAMLexport value ocaml_freetds_dbcanquery(value vdbproc)
{
  CAMLparam1(vdbproc);
  dbcanquery(DBPROCESS_VAL(vdbproc));
  CAMLreturn(Val_unit);
}


CAMLexport value ocaml_freetds_dbnextrow(value vdbproc)
{
  CAMLparam1(vdbproc);
  CAMLlocal4(vrow, vdata, vconstructor, vcons);
  DBPROCESS *dbproc = DBPROCESS_VAL(vdbproc);
  int c, ty, converted_len;
  BYTE *data;
  DBINT len;
  int data_int;
  char *data_char;
  double data_double;
  DBDATEREC di;

/* Taken from the implementation of caml_copy_string */
#define COPY_STRING(res, s, len_bytes)           \
  res = caml_alloc_string(len_bytes);                  \
  memmove(String_val(res), s, len_bytes);

#define CONVERT_STRING(destlen)                                         \
  data_char = malloc(destlen); /* printable size */                     \
  converted_len =                                                       \
    dbconvert(dbproc, ty, data, len,  SYBCHAR, (BYTE*) data_char, destlen); \
  if (converted_len < 0) {                                              \
    free(data_char);                                                    \
    raise_fatal("Freetds.Dblib.nextrow: problem with copying strings. " \
                "Please contact the author of the Freetds bindings.");  \
  } else {                                                              \
    COPY_STRING(vdata, data_char, converted_len);                       \
    free(data_char);                                                    \
  }

#define CONSTRUCTOR(tag, value)        \
  vconstructor = caml_alloc(1, tag);   \
  Store_field(vconstructor, 0, value)

  switch (dbnextrow(dbproc)) {
  case REG_ROW:
    vrow = Val_int(0); /* empty list [] */
    for (c = dbnumcols(dbproc); c >= 1; c--) {
      data = dbdata(dbproc, c); /* pointer to the data, no copy! */
      len = dbdatlen(dbproc, c); /* length, in bytes, of the data for
                                    a column. */
      if (len == 0 || data == NULL) {
        vconstructor = Val_int(0); /* constant constructor NULL */
      } else {
        switch (ty = dbcoltype(dbproc, c)) {
        case SYBCHAR:    /* fall-through */
        case SYBVARCHAR:
        case SYBTEXT:
          COPY_STRING(vdata, data, len);
          CONSTRUCTOR(0, vdata);
          break;
        case SYBIMAGE:
        case SYBBINARY:
        case SYBVARBINARY:
          COPY_STRING(vdata, data, len);
          CONSTRUCTOR(10, vdata);
          break;

        case SYBINT1:
          dbconvert(dbproc, ty, data, len,
                    SYBINT4, (BYTE*) &data_int, sizeof(int));
          CONSTRUCTOR(1, Val_int(data_int));
          break;
        case SYBINT2:
          dbconvert(dbproc, ty, data, len,
                    SYBINT4, (BYTE*) &data_int, sizeof(int));
          CONSTRUCTOR(2, Val_int(data_int));
          break;
        case SYBINTN:
        case SYBINT4:
          data_int = *((int *) data);
#if OCAML_WORD_SIZE == 32
          if (-1073741824 <= data_int && data_int < 1073741824)
            CONSTRUCTOR(3, Val_int(data_int));
          else /* require more than 31 bits allow for */
            CONSTRUCTOR(4, caml_copy_int32(data_int));
#else
          CONSTRUCTOR(3, Val_int(data_int));
#endif
          break;
        case SYBINT8:
          CONVERT_STRING(21);
          CONSTRUCTOR(5, vdata);
          break;

        case SYBFLT8:
          CONSTRUCTOR(6, caml_copy_double(* (double *) data));
          break;
        case SYBFLTN:
        case SYBREAL:
          dbconvert(dbproc, ty, data, len,  SYBFLT8,
                    (BYTE*) &data_double, sizeof(double));
          CONSTRUCTOR(6, caml_copy_double(data_double));
          break;

        case SYBNUMERIC:
          CONVERT_STRING(ceil(2.5 * len)); /* FIXME: max size ? */
          CONSTRUCTOR(11, vdata);
          break;
        case SYBDECIMAL:
          CONVERT_STRING(ceil(2.5 * len)); /* FIXME: max size ? */
          CONSTRUCTOR(12, vdata);
          break;

        case SYBBIT:
          CONSTRUCTOR(9, Val_bool(*data != '\0'));
          break;

        case SYBDATETIME:
        case SYBDATETIME4:
        case SYBDATETIMN:
          if (dbdatecrack(dbproc, &di, (DBDATETIME *) data) == FAIL) {
            raise_fatal("Freetds.Dblib.nextrow: date conversion failed. "
                        "Please contact the author of these bindings.");
          }
          vconstructor = caml_alloc(/* size: */ 8, /* tag: */ 7);
#ifdef MSDBLIB
         /* http://msdn.microsoft.com/en-us/library/aa937027%28SQL.80%29.aspx */
          Store_field(vconstructor, 0, Val_int(di.year));
          Store_field(vconstructor, 1, Val_int(di.month));
          Store_field(vconstructor, 2, Val_int(di.day));
          Store_field(vconstructor, 3, Val_int(di.hour));
          Store_field(vconstructor, 4, Val_int(di.minute));
          Store_field(vconstructor, 5, Val_int(di.second));
          Store_field(vconstructor, 6, Val_int(di.millisecond));
          Store_field(vconstructor, 7, Val_int(di.tzone));
#else
          /* From sybdb.h */
          Store_field(vconstructor, 0, Val_int(di.dateyear));
          Store_field(vconstructor, 1, Val_int(di.datemonth));
          Store_field(vconstructor, 2, Val_int(di.datedmonth));
          Store_field(vconstructor, 3, Val_int(di.datehour));
          Store_field(vconstructor, 4, Val_int(di.dateminute));
          Store_field(vconstructor, 5, Val_int(di.datesecond));
          Store_field(vconstructor, 6, Val_int(di.datemsecond));
          Store_field(vconstructor, 7, Val_int(di.datetzone));
#endif
          break;

        case SYBMONEY4:
        case SYBMONEY:
        case SYBMONEYN:
          dbconvert(dbproc, ty, data, len,  SYBFLT8,
                    (BYTE*) &data_double, sizeof(double));
          CONSTRUCTOR(8, caml_copy_double(data_double));
          break;

        default:
          printf("Freetds.Dblib.nextrow: dbcoltype not handled (C stub)\n");
          exit(1);
        }
      }
      /* Place the data in front of the list [vrow]. */
      vcons = alloc_tuple(2);
      Store_field(vcons, 0, vconstructor);
      Store_field(vcons, 1, vrow);
      vrow = vcons;
    }
    CAMLreturn(vrow);
    break;

  case NO_MORE_ROWS:
    caml_raise_not_found();
    break;

  case FAIL:
    /* The handler should be triggered (this should not occur). */
    raise_fatal("Freetds.Dblib.nextrow: failed");
    break;

  case BUF_FULL:
    raise_fatal("Freetds.Dblib.nextrow: buffer full (report to the developer "
                "of this library)");
    break;

  default:
    /* FIXME: compute rows are ignored */
    CAMLreturn(Val_int(0)); /* row = [] */
  }
}


CAMLexport value ocaml_freetds_dbcount(value vdbproc)
{
  CAMLparam1(vdbproc);
  CAMLreturn(Val_int(DBPROCESS_VAL(vdbproc)));
}


CAMLexport value ocaml_freetds_dbsettime(value vseconds)
{
  CAMLparam1(vseconds);
  /* http://manuals.sybase.com/onlinebooks/group-cnarc/cng1110e/dblib/@ebt-link;pt=16561?target=%25N%15_39241_START_RESTART_N%25 */
  if (dbsettime(Int_val(vseconds)) == FAIL) {
    raise_fatal("Freetds.Dblib.settime");
  }
  CAMLreturn(Val_unit);
}
