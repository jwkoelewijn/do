#include <libpq-fe.h>
#include <postgres.h>
#include <mb/pg_wchar.h>
#include <catalog/pg_type.h>

/* Undefine constants Postgres also defines */
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#ifdef _WIN32
/* On Windows this stuff is also defined by Postgres, but we don't
   want to use Postgres' version actually */
#undef fsync
#undef vsnprintf
#undef snprintf
#undef sprintf
#undef printf
#define cCommand_execute cCommand_execute_sync
#define do_int64 signed __int64
#else
#define cCommand_execute cCommand_execute_async
#define do_int64 signed long long int
#endif

#include <ruby.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#define ID_CONST_GET rb_intern("const_get")
#define ID_PATH rb_intern("path")
#define ID_NEW rb_intern("new")
#define ID_ESCAPE rb_intern("escape_sql")

#define RUBY_STRING(char_ptr) rb_str_new2(char_ptr)
#define TAINTED_STRING(name, length) rb_tainted_str_new(name, length)
#define CONST_GET(scope, constant) (rb_funcall(scope, ID_CONST_GET, 1, rb_str_new2(constant)))
#define POSTGRES_CLASS(klass, parent) (rb_define_class_under(mPostgres, klass, parent))
#define DEBUG(value) data_objects_debug(value)
#define RUBY_CLASS(name) rb_const_get(rb_cObject, rb_intern(name))

#ifndef RSTRING_PTR
#define RSTRING_PTR(s) (RSTRING(s)->ptr)
#endif

#ifndef RSTRING_LEN
#define RSTRING_LEN(s) (RSTRING(s)->len)
#endif

#ifndef RARRAY_LEN
#define RARRAY_LEN(a) RARRAY(a)->len
#endif


// To store rb_intern values
static ID ID_NEW_DATE;
static ID ID_LOGGER;
static ID ID_DEBUG;
static ID ID_LEVEL;
static ID ID_TO_S;
static ID ID_RATIONAL;

static VALUE mExtlib;
static VALUE mDO;
static VALUE cDO_Quoting;
static VALUE cDO_Connection;
static VALUE cDO_Command;
static VALUE cDO_Result;
static VALUE cDO_Reader;

static VALUE rb_cDate;
static VALUE rb_cDateTime;
static VALUE rb_cBigDecimal;
static VALUE rb_cByteArray;

static VALUE mPostgres;
static VALUE cConnection;
static VALUE cCommand;
static VALUE cResult;
static VALUE cReader;

static VALUE eArgumentError;
static VALUE ePostgresError;

static void data_objects_debug(VALUE string, struct timeval* start) {
  struct timeval stop;
  char *message;

  char *query = RSTRING_PTR(string);
  int length  = RSTRING_LEN(string);
  char total_time[32];
  do_int64 duration = 0;

  VALUE logger = rb_funcall(mPostgres, ID_LOGGER, 0);
  int log_level = NUM2INT(rb_funcall(logger, ID_LEVEL, 0));

  if (0 == log_level) {
    gettimeofday(&stop, NULL);

    duration = (stop.tv_sec - start->tv_sec) * 1000000 + stop.tv_usec - start->tv_usec;

    snprintf(total_time, 32, "%.6f", duration / 1000000.0);
    message = (char *)calloc(length + strlen(total_time) + 4, sizeof(char));
    snprintf(message, length + strlen(total_time) + 4, "(%s) %s", total_time, query);
    rb_funcall(logger, ID_DEBUG, 1, rb_str_new(message, length + strlen(total_time) + 3));
  }
}

static char * get_uri_option(VALUE query_hash, char * key) {
  VALUE query_value;
  char * value = NULL;

  if(!rb_obj_is_kind_of(query_hash, rb_cHash)) { return NULL; }

  query_value = rb_hash_aref(query_hash, RUBY_STRING(key));

  if (Qnil != query_value) {
    value = StringValuePtr(query_value);
  }

  return value;
}

/* ====== Time/Date Parsing Helper Functions ====== */
static void reduce( do_int64 *numerator, do_int64 *denominator ) {
  do_int64 a, b, c;
  a = *numerator;
  b = *denominator;
  while ( a != 0 ) {
    c = a; a = b % a; b = c;
  }
  *numerator = *numerator / b;
  *denominator = *denominator / b;
}

// Generate the date integer which Date.civil_to_jd returns
static int jd_from_date(int year, int month, int day) {
  int a, b;
  if ( month <= 2 ) {
    year -= 1;
    month += 12;
  }
  a = year / 100;
  b = 2 - a + (a / 4);
  return floor(365.25 * (year + 4716)) + floor(30.6001 * (month + 1)) + day + b - 1524;
}

static VALUE parse_date(const char *date) {
  int year, month, day;
  int jd, ajd;
  VALUE rational;

  sscanf(date, "%4d-%2d-%2d", &year, &month, &day);

  return rb_funcall(rb_cDate, ID_NEW, 3, INT2NUM(year), INT2NUM(month), INT2NUM(day));
}

// Creates a Rational for use as a Timezone offset to be passed to DateTime.new!
static VALUE seconds_to_offset(do_int64 num) {
  do_int64 den = 86400;
  reduce(&num, &den);
  return rb_funcall(rb_mKernel, ID_RATIONAL, 2, rb_ll2inum(num), rb_ll2inum(den));
}

static VALUE timezone_to_offset(int hour_offset, int minute_offset) {
  do_int64 seconds = 0;

  seconds += hour_offset * 3600;
  seconds += minute_offset * 60;

  return seconds_to_offset(seconds);
}

static VALUE parse_date_time(const char *date) {
  VALUE ajd, offset;

  int year, month, day, hour, min, sec, usec, hour_offset, minute_offset;
  int jd;
  do_int64 num, den;

  long int gmt_offset;
  int is_dst;

  time_t rawtime;
  struct tm * timeinfo;

  int tokens_read, max_tokens;

  if (0 != strchr(date, '.')) {
    // This is a datetime with sub-second precision
    tokens_read = sscanf(date, "%4d-%2d-%2d %2d:%2d:%2d.%d%3d:%2d", &year, &month, &day, &hour, &min, &sec, &usec, &hour_offset, &minute_offset);
    max_tokens = 9;
  } else {
    // This is a datetime second precision
    tokens_read = sscanf(date, "%4d-%2d-%2d %2d:%2d:%2d%3d:%2d", &year, &month, &day, &hour, &min, &sec, &hour_offset, &minute_offset);
    max_tokens = 8;
  }

  if (max_tokens == tokens_read) {
    // We read the Date, Time, and Timezone info
    minute_offset *= hour_offset < 0 ? -1 : 1;
  } else if ((max_tokens - 1) == tokens_read) {
    // We read the Date and Time, but no Minute Offset
    minute_offset = 0;
  } else if (tokens_read == 3 || tokens_read >= (max_tokens - 3)) {
    if (tokens_read == 3) {
      hour = 0;
      min = 0;
      hour_offset = 0;
      minute_offset = 0;
      sec = 0;
    }  
    // We read the Date and Time, default to the current locale's offset

    // Get localtime
    time(&rawtime);
    timeinfo = localtime(&rawtime);

    is_dst = timeinfo->tm_isdst * 3600;

    // Reset to GM Time
    timeinfo = gmtime(&rawtime);

    gmt_offset = mktime(timeinfo) - rawtime;

    if ( is_dst > 0 )
      gmt_offset -= is_dst;

    hour_offset = -(gmt_offset / 3600);
    minute_offset = -(gmt_offset % 3600 / 60);

  } else {
    // Something went terribly wrong
    rb_raise(ePostgresError, "Couldn't parse date: %s", date);
  }

  offset = timezone_to_offset(hour_offset, minute_offset);

  return rb_funcall(rb_cDateTime, ID_NEW, 7, INT2NUM(year),
                                  INT2NUM(month), INT2NUM(day),
                                  INT2NUM(hour),
                                  INT2NUM(min),
                                  INT2NUM(sec), offset);
}

static VALUE parse_time(const char *date) {

  int year, month, day, hour, min, sec, usec, tokens;
  char subsec[7];

  if (0 != strchr(date, '.')) {
    // right padding usec with 0. e.g. '012' will become 12000 microsecond, since Time#local use microsecond
    sscanf(date, "%4d-%2d-%2d %2d:%2d:%2d.%s", &year, &month, &day, &hour, &min, &sec, subsec);
    usec   = atoi(subsec);
    usec  *= pow(10, (6 - strlen(subsec)));
  } else {
    tokens = sscanf(date, "%4d-%2d-%2d %2d:%2d:%2d", &year, &month, &day, &hour, &min, &sec);
    if (tokens == 3) {
      hour = 0;
      min  = 0;
      sec  = 0;
    }
    usec = 0;
  }

  return rb_funcall(rb_cTime, rb_intern("local"), 7, INT2NUM(year), INT2NUM(month), INT2NUM(day), INT2NUM(hour), INT2NUM(min), INT2NUM(sec), INT2NUM(usec));
}

/* ===== Typecasting Functions ===== */

static VALUE infer_ruby_type(Oid type) {
  switch(type) {
    case BITOID:
    case VARBITOID:
    case INT2OID:
    case INT4OID:
    case INT8OID:
      return rb_cInteger;
    case FLOAT4OID:
    case FLOAT8OID:
      return rb_cFloat;
    case NUMERICOID:
    case CASHOID:
      return rb_cBigDecimal;
    case BOOLOID:
      return rb_cTrueClass;
    case TIMESTAMPTZOID:
    case TIMESTAMPOID:
      return rb_cDateTime;
    case DATEOID:
      return rb_cDate;
    case BYTEAOID:
      return rb_cByteArray;
    default:
      return rb_cString;
  }
}

static VALUE typecast(const char *value, long length, const VALUE type) {

  if (type == rb_cInteger) {
    return rb_cstr2inum(value, 10);
  } else if (type == rb_cString) {
    return TAINTED_STRING(value, length);
  } else if (type == rb_cFloat) {
    return rb_float_new(rb_cstr_to_dbl(value, Qfalse));
  } else if (type == rb_cBigDecimal) {
    return rb_funcall(rb_cBigDecimal, ID_NEW, 1, TAINTED_STRING(value, length));
  } else if (type == rb_cDate) {
    return parse_date(value);
  } else if (type == rb_cDateTime) {
    return parse_date_time(value);
  } else if (type == rb_cTime) {
    return parse_time(value);
  } else if (type == rb_cTrueClass) {
    return *value == 't' ? Qtrue : Qfalse;
  } else if (type == rb_cByteArray) {
    size_t new_length = 0;
    char* unescaped = (char *)PQunescapeBytea((unsigned char*)value, &new_length);
    VALUE byte_array = rb_funcall(rb_cByteArray, ID_NEW, 1, TAINTED_STRING(unescaped, new_length));
    PQfreemem(unescaped);
    return byte_array;
  } else if (type == rb_cClass) {
    return rb_funcall(rb_cObject, rb_intern("full_const_get"), 1, TAINTED_STRING(value, length));
  } else if (type == rb_cObject) {
    return rb_marshal_load(rb_str_new2(value));
  } else if (type == rb_cNilClass) {
    return Qnil;
  } else {
    return TAINTED_STRING(value, length);
  }

}

/* ====== Public API ======= */
static VALUE cConnection_dispose(VALUE self) {
  VALUE connection_container = rb_iv_get(self, "@connection");

  PGconn *db;

  if (Qnil == connection_container)
    return Qfalse;

  db = DATA_PTR(connection_container);

  if (NULL == db)
    return Qfalse;

  PQfinish(db);
  rb_iv_set(self, "@connection", Qnil);

  return Qtrue;
}

static VALUE cCommand_set_types(int argc, VALUE *argv, VALUE self) {
  VALUE type_strings = rb_ary_new();
  VALUE array = rb_ary_new();

  int i, j;

  for ( i = 0; i < argc; i++) {
    rb_ary_push(array, argv[i]);
  }

  for (i = 0; i < RARRAY_LEN(array); i++) {
    VALUE entry = rb_ary_entry(array, i);
    if(TYPE(entry) == T_CLASS) {
      rb_ary_push(type_strings, entry);
    } else if (TYPE(entry) == T_ARRAY) {
      for (j = 0; j < RARRAY_LEN(entry); j++) {
        VALUE sub_entry = rb_ary_entry(entry, j);
        if(TYPE(sub_entry) == T_CLASS) {
          rb_ary_push(type_strings, sub_entry);
        } else {
          rb_raise(eArgumentError, "Invalid type given");
        }
      }
    } else {
      rb_raise(eArgumentError, "Invalid type given");
    }
  }

  rb_iv_set(self, "@field_types", type_strings);

  return array;
}

static VALUE build_query_from_args(VALUE klass, int count, VALUE *args[]) {
  VALUE query = rb_iv_get(klass, "@text");

  int i;
  VALUE array = rb_ary_new();
  for ( i = 0; i < count; i++) {
    rb_ary_push(array, (VALUE)args[i]);
  }
  query = rb_funcall(klass, ID_ESCAPE, 1, array);

  return query;
}

static VALUE cConnection_quote_string(VALUE self, VALUE string) {
  PGconn *db = DATA_PTR(rb_iv_get(self, "@connection"));

  const char *source = RSTRING_PTR(string);
  int source_len     = RSTRING_LEN(string);

  char *escaped;
  int quoted_length = 0;
  VALUE result;

  // Allocate space for the escaped version of 'string'
  // http://www.postgresql.org/docs/8.3/static/libpq-exec.html#LIBPQ-EXEC-ESCAPE-STRING
  escaped = (char *)calloc(source_len * 2 + 3, sizeof(char));

  // Escape 'source' using the current charset in use on the conection 'db'
  quoted_length = PQescapeStringConn(db, escaped + 1, source, source_len, NULL);

  // Wrap the escaped string in single-quotes, this is DO's convention
  escaped[quoted_length + 1] = escaped[0] = '\'';

  result = rb_str_new(escaped, quoted_length + 2);
  free(escaped);
  return result;
}

static VALUE cConnection_quote_byte_array(VALUE self, VALUE string) {
  PGconn *db = DATA_PTR(rb_iv_get(self, "@connection"));

  const unsigned char *source = (unsigned char*) RSTRING_PTR(string);
  size_t source_len     = RSTRING_LEN(string);

  unsigned char *escaped;
  unsigned char *escaped_quotes;
  size_t quoted_length = 0;
  VALUE result;

  // Allocate space for the escaped version of 'string'
  // http://www.postgresql.org/docs/8.3/static/libpq-exec.html#LIBPQ-EXEC-ESCAPE-STRING
  escaped = PQescapeByteaConn(db, source, source_len, &quoted_length);
  escaped_quotes = (unsigned char *)calloc(quoted_length + 1, sizeof(unsigned char));
  memcpy(escaped_quotes + 1, escaped, quoted_length);

  // Wrap the escaped string in single-quotes, this is DO's convention (replace trailing \0)
  escaped_quotes[quoted_length] = escaped_quotes[0] = '\'';

  result = TAINTED_STRING((char*)escaped_quotes, quoted_length + 1);
  PQfreemem(escaped);
  free(escaped_quotes);
  return result;
}

#ifdef _WIN32
static PGresult* cCommand_execute_sync(PGconn *db, VALUE query) {
  PGresult *response;
  struct timeval start;
  char* str = StringValuePtr(query);

  while ((response = PQgetResult(db)) != NULL) {
    PQclear(response);
  }

  gettimeofday(&start, NULL);

  response = PQexec(db, str);

  if (response == NULL) {
    if(PQstatus(db) != CONNECTION_OK) {
      PQreset(db);
      if (PQstatus(db) == CONNECTION_OK) {
        response = PQexec(db, str);
      }
    }

    if(response == NULL) {
      rb_raise(ePostgresError, PQerrorMessage(db));
    }
  }

  data_objects_debug(query, &start);
  return response;
}
#else
static PGresult* cCommand_execute_async(PGconn *db, VALUE query) {
  int socket_fd;
  int retval;
  fd_set rset;
  PGresult *response;
  struct timeval start;
  char* str = StringValuePtr(query);

  while ((response = PQgetResult(db)) != NULL) {
    PQclear(response);
  }

  retval = PQsendQuery(db, str);

  if (!retval) {
    if(PQstatus(db) != CONNECTION_OK) {
      PQreset(db);
      if (PQstatus(db) == CONNECTION_OK) {
        retval = PQsendQuery(db, str);
      }
    }

    if(!retval) {
      rb_raise(ePostgresError, PQerrorMessage(db));
    }
  }

  gettimeofday(&start, NULL);
  socket_fd = PQsocket(db);

  for(;;) {
      FD_ZERO(&rset);
      FD_SET(socket_fd, &rset);
      retval = rb_thread_select(socket_fd + 1, &rset, NULL, NULL, NULL);
      if (retval < 0) {
          rb_sys_fail(0);
      }

      if (retval == 0) {
          continue;
      }

      if (PQconsumeInput(db) == 0) {
          rb_raise(ePostgresError, PQerrorMessage(db));
      }

      if (PQisBusy(db) == 0) {
          break;
      }
  }

  data_objects_debug(query, &start);
  return PQgetResult(db);
}
#endif

static VALUE cConnection_initialize(VALUE self, VALUE uri) {
  PGresult *result = NULL;
  VALUE r_host, r_user, r_password, r_path, r_port, r_query, r_options;
  char *host = NULL, *user = NULL, *password = NULL, *path;
  char *database = "", *port = "5432";
  char *encoding = NULL;
  char *search_path = NULL;
  char *search_path_query = NULL;
  char *backslash_off = "SET backslash_quote = off";
  char *standard_strings_on = "SET standard_conforming_strings = on";
  char *warning_messages = "SET client_min_messages = warning";

  PGconn *db;

  r_host = rb_funcall(uri, rb_intern("host"), 0);
  if ( Qnil != r_host ) {
    host = StringValuePtr(r_host);
  }

  r_user = rb_funcall(uri, rb_intern("user"), 0);
  if (Qnil != r_user) {
    user = StringValuePtr(r_user);
  }

  r_password = rb_funcall(uri, rb_intern("password"), 0);
  if (Qnil != r_password) {
    password = StringValuePtr(r_password);
  }

  r_path = rb_funcall(uri, rb_intern("path"), 0);
  path = StringValuePtr(r_path);
  if (Qnil != r_path) {
    database = strtok(path, "/");
  }

  if (NULL == database || 0 == strlen(database)) {
    rb_raise(ePostgresError, "Database must be specified");
  }

  r_port = rb_funcall(uri, rb_intern("port"), 0);
  if (Qnil != r_port) {
    r_port = rb_funcall(r_port, rb_intern("to_s"), 0);
    port = StringValuePtr(r_port);
  }

  // Pull the querystring off the URI
  r_query = rb_funcall(uri, rb_intern("query"), 0);

  search_path = get_uri_option(r_query, "search_path");

  db = PQsetdbLogin(
    host,
    port,
    NULL,
    NULL,
    database,
    user,
    password
  );

  if ( PQstatus(db) == CONNECTION_BAD ) {
    rb_raise(ePostgresError, PQerrorMessage(db));
  }

  if (search_path != NULL) {
    search_path_query = (char *)calloc(256, sizeof(char));
    snprintf(search_path_query, 256, "set search_path to %s;", search_path);
    r_query = rb_str_new2(search_path_query);
    result = cCommand_execute(db, r_query);

    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
      free(search_path_query);
      rb_raise(ePostgresError, PQresultErrorMessage(result));
    }

    free(search_path_query);
  }

  r_options = rb_str_new2(backslash_off);
  result = cCommand_execute(db, r_options);

  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    rb_warn(PQresultErrorMessage(result));
  }

  r_options = rb_str_new2(standard_strings_on);
  result = cCommand_execute(db, r_options);

  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    rb_warn(PQresultErrorMessage(result));
  }

  r_options = rb_str_new2(warning_messages);
  result = cCommand_execute(db, r_options);

  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    rb_warn(PQresultErrorMessage(result));
  }

  encoding = get_uri_option(r_query, "encoding");
  if (!encoding) { encoding = get_uri_option(r_query, "charset"); }
  if (!encoding) { encoding = "utf8"; }

#ifdef HAVE_PQSETCLIENTENCODING
  if(PQsetClientEncoding(db, encoding)) {
    rb_raise(ePostgresError, "Couldn't set encoding: %s", encoding);
  }
#endif

  rb_iv_set(self, "@uri", uri);
  rb_iv_set(self, "@connection", Data_Wrap_Struct(rb_cObject, 0, 0, db));

  return Qtrue;
}

static VALUE cConnection_character_set(VALUE self) {
  VALUE connection_container = rb_iv_get(self, "@connection");
  PGconn *db;

  const char *encoding;

  if (Qnil == connection_container)
    return Qfalse;

  db = DATA_PTR(connection_container);

  encoding = pg_encoding_to_char(PQclientEncoding(db));

  return rb_funcall(RUBY_STRING(encoding), rb_intern("downcase"), 0);
}

static VALUE cCommand_execute_non_query(int argc, VALUE *argv[], VALUE self) {
  VALUE connection = rb_iv_get(self, "@connection");
  VALUE postgres_connection = rb_iv_get(connection, "@connection");
  if (Qnil == postgres_connection) {
    rb_raise(ePostgresError, "This connection has already been closed.");
  }

  PGconn *db = DATA_PTR(postgres_connection);
  PGresult *response;
  int status;

  VALUE affected_rows = Qnil;
  VALUE insert_id = Qnil;

  VALUE query = build_query_from_args(self, argc, argv);

  response = cCommand_execute(db, query);

  status = PQresultStatus(response);

  if ( status == PGRES_TUPLES_OK ) {
    insert_id = INT2NUM(atoi(PQgetvalue(response, 0, 0)));
    affected_rows = INT2NUM(atoi(PQcmdTuples(response)));
  }
  else if ( status == PGRES_COMMAND_OK ) {
    insert_id = Qnil;
    affected_rows = INT2NUM(atoi(PQcmdTuples(response)));
  }
  else {
    char *message = PQresultErrorMessage(response);
    char *sqlstate = PQresultErrorField(response, PG_DIAG_SQLSTATE);
    PQclear(response);
    rb_raise(ePostgresError, "(sql_state=%s) %sQuery: %s\n", sqlstate, message, StringValuePtr(query));
  }

  PQclear(response);

  return rb_funcall(cResult, ID_NEW, 3, self, affected_rows, insert_id);
}

static VALUE cCommand_execute_reader(int argc, VALUE *argv[], VALUE self) {
  VALUE reader, query;
  VALUE field_names, field_types;

  int i;
  int field_count;
  int infer_types = 0;

  VALUE connection = rb_iv_get(self, "@connection");
  VALUE postgres_connection = rb_iv_get(connection, "@connection");
  if (Qnil == postgres_connection) {
    rb_raise(ePostgresError, "This connection has already been closed.");
  }

  PGconn *db = DATA_PTR(postgres_connection);
  PGresult *response;

  query = build_query_from_args(self, argc, argv);

  response = cCommand_execute(db, query);

  if ( PQresultStatus(response) != PGRES_TUPLES_OK ) {
    char *message = PQresultErrorMessage(response);
    PQclear(response);
    rb_raise(ePostgresError, "%sQuery: %s\n", message, StringValuePtr(query));
  }

  field_count = PQnfields(response);

  reader = rb_funcall(cReader, ID_NEW, 0);
  rb_iv_set(reader, "@reader", Data_Wrap_Struct(rb_cObject, 0, 0, response));
  rb_iv_set(reader, "@field_count", INT2NUM(field_count));
  rb_iv_set(reader, "@row_count", INT2NUM(PQntuples(response)));

  field_names = rb_ary_new();
  field_types = rb_iv_get(self, "@field_types");

  if ( field_types == Qnil || 0 == RARRAY_LEN(field_types) ) {
    field_types = rb_ary_new();
    infer_types = 1;
  } else if (RARRAY_LEN(field_types) != field_count) {
    // Whoops...  wrong number of types passed to set_types.  Close the reader and raise
    // and error
    rb_funcall(reader, rb_intern("close"), 0);
    rb_raise(eArgumentError, "Field-count mismatch. Expected %ld fields, but the query yielded %d", RARRAY_LEN(field_types), field_count);
  }

  for ( i = 0; i < field_count; i++ ) {
    rb_ary_push(field_names, rb_str_new2(PQfname(response, i)));
    if ( infer_types == 1 ) {
      rb_ary_push(field_types, infer_ruby_type(PQftype(response, i)));
    }
  }

  rb_iv_set(reader, "@position", INT2NUM(0));
  rb_iv_set(reader, "@fields", field_names);
  rb_iv_set(reader, "@field_types", field_types);

  return reader;
}

static VALUE cReader_close(VALUE self) {
  VALUE reader_container = rb_iv_get(self, "@reader");

  PGresult *reader;

  if (Qnil == reader_container)
    return Qfalse;

  reader = DATA_PTR(reader_container);

  if (NULL == reader)
    return Qfalse;

  PQclear(reader);
  rb_iv_set(self, "@reader", Qnil);
  return Qtrue;
}

static VALUE cReader_next(VALUE self) {
  PGresult *reader = DATA_PTR(rb_iv_get(self, "@reader"));

  int field_count;
  int row_count;
  int i;
  int position;

  VALUE array = rb_ary_new();
  VALUE field_types, field_type;
  VALUE value;

  row_count = NUM2INT(rb_iv_get(self, "@row_count"));
  field_count = NUM2INT(rb_iv_get(self, "@field_count"));
  field_types = rb_iv_get(self, "@field_types");
  position = NUM2INT(rb_iv_get(self, "@position"));

  if ( position > (row_count-1) ) {
    rb_iv_set(self, "@values", Qnil);
    return Qfalse;
  }

  for ( i = 0; i < field_count; i++ ) {
    field_type = rb_ary_entry(field_types, i);

    // Always return nil if the value returned from Postgres is null
    if (!PQgetisnull(reader, position, i)) {
      value = typecast(PQgetvalue(reader, position, i), PQgetlength(reader, position, i), field_type);
    } else {
      value = Qnil;
    }

    rb_ary_push(array, value);
  }

  rb_iv_set(self, "@values", array);
  rb_iv_set(self, "@position", INT2NUM(position+1));

  return Qtrue;
}

static VALUE cReader_values(VALUE self) {

  VALUE values = rb_iv_get(self, "@values");
  if(values == Qnil) {
    rb_raise(ePostgresError, "Reader not initialized");
    return Qnil;
  } else {
    return values;
  }
}

static VALUE cReader_fields(VALUE self) {
  return rb_iv_get(self, "@fields");
}

static VALUE cReader_field_count(VALUE self) {
  return rb_iv_get(self, "@field_count");
}

void Init_do_postgres_ext() {
  rb_require("date");
  rb_require("bigdecimal");

  // Get references classes needed for Date/Time parsing
  rb_cDate = CONST_GET(rb_mKernel, "Date");
  rb_cDateTime = CONST_GET(rb_mKernel, "DateTime");
  rb_cBigDecimal = CONST_GET(rb_mKernel, "BigDecimal");

  rb_funcall(rb_mKernel, rb_intern("require"), 1, rb_str_new2("data_objects"));

#ifdef RUBY_LESS_THAN_186
  ID_NEW_DATE = rb_intern("new0");
#else
  ID_NEW_DATE = rb_intern("new!");
#endif
  ID_LOGGER = rb_intern("logger");
  ID_DEBUG = rb_intern("debug");
  ID_LEVEL = rb_intern("level");
  ID_TO_S = rb_intern("to_s");
  ID_RATIONAL = rb_intern("Rational");

  // Get references to the Extlib module
  mExtlib = CONST_GET(rb_mKernel, "Extlib");
  rb_cByteArray = CONST_GET(mExtlib, "ByteArray");

  // Get references to the DataObjects module and its classes
  mDO = CONST_GET(rb_mKernel, "DataObjects");
  cDO_Quoting = CONST_GET(mDO, "Quoting");
  cDO_Connection = CONST_GET(mDO, "Connection");
  cDO_Command = CONST_GET(mDO, "Command");
  cDO_Result = CONST_GET(mDO, "Result");
  cDO_Reader = CONST_GET(mDO, "Reader");

  eArgumentError = CONST_GET(rb_mKernel, "ArgumentError");
  mPostgres = rb_define_module_under(mDO, "Postgres");
  ePostgresError = rb_define_class("PostgresError", rb_eStandardError);

  cConnection = POSTGRES_CLASS("Connection", cDO_Connection);
  rb_define_method(cConnection, "initialize", cConnection_initialize, 1);
  rb_define_method(cConnection, "dispose", cConnection_dispose, 0);
  rb_define_method(cConnection, "character_set", cConnection_character_set , 0);
  rb_define_method(cConnection, "quote_string", cConnection_quote_string, 1);
  rb_define_method(cConnection, "quote_byte_array", cConnection_quote_byte_array, 1);

  cCommand = POSTGRES_CLASS("Command", cDO_Command);
  rb_define_method(cCommand, "set_types", cCommand_set_types, -1);
  rb_define_method(cCommand, "execute_non_query", cCommand_execute_non_query, -1);
  rb_define_method(cCommand, "execute_reader", cCommand_execute_reader, -1);

  cResult = POSTGRES_CLASS("Result", cDO_Result);

  cReader = POSTGRES_CLASS("Reader", cDO_Reader);
  rb_define_method(cReader, "close", cReader_close, 0);
  rb_define_method(cReader, "next!", cReader_next, 0);
  rb_define_method(cReader, "values", cReader_values, 0);
  rb_define_method(cReader, "fields", cReader_fields, 0);
  rb_define_method(cReader, "field_count", cReader_field_count, 0);

}
