
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "pyodbc.h"
#include "pyodbcmodule.h"
#include "connection.h"
#include "cursor.h"
#include "row.h"
#include "wrapper.h"
#include "errors.h"
#include "getdata.h"
#include "cnxninfo.h"
#include "dbspecific.h"

#include <time.h>
#include <stdarg.h>

_typeobject* OurDateTimeType = 0;
_typeobject* OurDateType = 0;
_typeobject* OurTimeType = 0;

PyObject* pModule = 0;

static char module_doc[] =
    "A DB API 2.0 module for ODBC databases.\n"
    "\n"
    "This module conforms to the DB API 2.0 specification while providing\n"
    "non-standard convenience features.  Only standard Python data types are used\n"
    "so additional DLLs are not required.\n"
    "\n"
    "Static Variables:\n\n"
    "version\n"
    "  The module version string in the format major.minor.revision\n"
    "\n"
    "apilevel\n"
    "  The string constant '2.0' indicating this module supports DB API level 2.0.\n"
    "\n"
    "lowercase\n"
    "  A Boolean that controls whether column names in result rows are lowercased.\n"
    "  This can be changed any time and affects queries executed after the change.\n"
    "  The default is False.  This can be useful when database columns have\n"
    "  inconsistent capitalization.\n"
    "pooling\n"
    "  A Boolean indicating whether connection pooling is enabled.  This is a\n"
    "  global (HENV) setting, so it can only be modified before the first\n"
    "  connection is made.  The default is True, which enables ODBC connection\n"
    "  pooling.\n"
    "\n"
    "threadsafety\n"
    "  The integer 1, indicating that threads may share the module but not\n"
    "  connections.  Note that connections and cursors may be used by different\n"
    "  threads, just not at the same time.\n"
    "\n"
    "qmark\n"
    "  The string constant 'qmark' to indicate parameters are identified using\n"
    "  question marks.";

PyObject* Error;
PyObject* Warning;
PyObject* InterfaceError;
PyObject* DatabaseError;
PyObject* InternalError;
PyObject* OperationalError;
PyObject* ProgrammingError;
PyObject* IntegrityError;
PyObject* DataError;
PyObject* NotSupportedError;

struct ExcInfo
{
    const char* szName;
    const char* szFullName;
    PyObject** ppexc;
    PyObject** ppexcParent;
    const char* szDoc;
};

#define MAKEEXCINFO(name, parent, doc) { #name, "pyodbc." #name, &name, &parent, doc }

static ExcInfo aExcInfos[] = {
    MAKEEXCINFO(Error, PyExc_StandardError, 
                "Exception that is the base class of all other error exceptions. You can use\n"
                "this to catch all errors with one single 'except' statement."),
    MAKEEXCINFO(Warning, PyExc_StandardError,
                "Exception raised for important warnings like data truncations while inserting,\n"
                " etc."),
    MAKEEXCINFO(InterfaceError, Error,
                "Exception raised for errors that are related to the database interface rather\n"
                "than the database itself."),
    MAKEEXCINFO(DatabaseError, Error, "Exception raised for errors that are related to the database."),
    MAKEEXCINFO(DataError, DatabaseError, 
                "Exception raised for errors that are due to problems with the processed data\n"
                "like division by zero, numeric value out of range, etc."),
    MAKEEXCINFO(OperationalError, DatabaseError,
                "Exception raised for errors that are related to the database's operation and\n"
                "not necessarily under the control of the programmer, e.g. an unexpected\n"
                "disconnect occurs, the data source name is not found, a transaction could not\n"
                "be processed, a memory allocation error occurred during processing, etc."),
    MAKEEXCINFO(IntegrityError, DatabaseError,
                "Exception raised when the relational integrity of the database is affected,\n"
                "e.g. a foreign key check fails."),
    MAKEEXCINFO(InternalError, DatabaseError,
                "Exception raised when the database encounters an internal error, e.g. the\n"
                "cursor is not valid anymore, the transaction is out of sync, etc."),
    MAKEEXCINFO(ProgrammingError, DatabaseError,
                "Exception raised for programming errors, e.g. table not found or already\n"
                "exists, syntax error in the SQL statement, wrong number of parameters\n"
                "specified, etc."),
    MAKEEXCINFO(NotSupportedError, DatabaseError,
                "Exception raised in case a method or database API was used which is not\n"
                "supported by the database, e.g. requesting a .rollback() on a connection that\n"
                "does not support transaction or has transactions turned off.")
};


PyObject* decimal_type;

HENV henv = SQL_NULL_HANDLE;

char chDecimal        = '.';
char chGroupSeparator = ',';
char chCurrencySymbol = '$';

// Initialize the global decimal character and thousands separator character, used when parsing decimal
// objects.
//
static void init_locale_info()
{
    Object module = PyImport_ImportModule("locale");
    if (!module)
    {
        PyErr_Clear();
        return;
    }

    Object ldict = PyObject_CallMethod(module, "localeconv", 0);
    if (!ldict)
    {
        PyErr_Clear();
        return;
    }

    PyObject* value = PyDict_GetItemString(ldict, "decimal");
    if (value && PyString_Check(value) && PyString_Size(value) == 1)
    {
        chDecimal = PyString_AsString(value)[0];
    }
        
    value = PyDict_GetItemString(ldict, "thousands_sep");
    if (value && PyString_Check(value) && PyString_Size(value) == 1)
    {
        chGroupSeparator = PyString_AsString(value)[0];

        if (chGroupSeparator == '\0')
        {
            // I don't know why, but the default locale isn't setting ','.  We're going to make the assumption that the
            // most common values are ',' and '.', and we'll take the opposite of the decimal value.
            chGroupSeparator = (chDecimal == ',') ? '.' : ',';
        }
    }

    value = PyDict_GetItemString(ldict, "currency_symbol");
    if (value && PyString_Check(value) && PyString_Size(value) == 1)
    {
        chCurrencySymbol = PyString_AsString(value)[0];
    }
}


static bool import_types()
{
    // In Python 2.5 final, PyDateTime_IMPORT no longer works unless the datetime module was previously
    // imported (among other problems).

    PyObject* pdt = PyImport_ImportModule("datetime");
    
    if (!pdt)
        return false;

    PyDateTime_IMPORT;
     
    if (!PyDateTimeAPI)
    {
        PyErr_SetString(PyExc_RuntimeError, "Unable to import the datetime module.");
        return false;
    }
    
    OurDateTimeType = PyDateTimeAPI->DateTimeType;
    OurDateType     = PyDateTimeAPI->DateType;
    OurTimeType     = PyDateTimeAPI->TimeType;

    Cursor_init();
    CnxnInfo_init();
    GetData_init();

    PyObject* decimalmod = PyImport_ImportModule("decimal");
    if (!decimalmod)
    {
        PyErr_SetString(PyExc_RuntimeError, "Unable to import decimal");
        return false;
    }
    
    decimal_type = PyObject_GetAttrString(decimalmod, "Decimal");
    Py_DECREF(decimalmod);

    if (decimal_type == 0)
        PyErr_SetString(PyExc_RuntimeError, "Unable to import decimal.Decimal.");
    
    return decimal_type != 0;
}


static bool AllocateEnv()
{
    PyObject* pooling = PyObject_GetAttrString(pModule, "pooling");
    bool bPooling = pooling == Py_True;
    Py_DECREF(pooling);

    if (bPooling)
    {
        if (!SQL_SUCCEEDED(SQLSetEnvAttr(SQL_NULL_HANDLE, SQL_ATTR_CONNECTION_POOLING, (SQLPOINTER)SQL_CP_ONE_PER_HENV, sizeof(int))))
        {
            Py_FatalError("Unable to set SQL_ATTR_CONNECTION_POOLING attribute.");
            return false;
        }
    }
    
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv)))
    {
        Py_FatalError("Can't initialize module pyodbc.  SQLAllocEnv failed.");
        return false;
    }

    if (!SQL_SUCCEEDED(SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, sizeof(int))))
    {
        Py_FatalError("Unable to set SQL_ATTR_ODBC_VERSION attribute.");
        return false;
    }

    return true;
}

char* connect_kwnames[] = { "connectstring", "autocommit", "ansi", 0 };

static PyObject*
mod_connect(PyObject* self, PyObject* args, PyObject* kwargs)
{
    UNUSED(self);
    
    PyObject* pConnectString;   // Can be str or unicode.
    int fAutoCommit = 0;
    int fAnsi = 0;              // force ansi
    
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|ii", connect_kwnames, &pConnectString, &fAutoCommit, &fAnsi))
        return 0;

    if (!PyString_Check(pConnectString) && !PyUnicode_Check(pConnectString))
    {
        PyErr_SetString(PyExc_TypeError, "argument 1 must be a string or unicode object");
        return 0;
    }

    if (henv == SQL_NULL_HANDLE)
    {
        if (!AllocateEnv())
            return 0;
    }

    return (PyObject*)Connection_New(pConnectString, fAutoCommit != 0, fAnsi != 0);
}


static PyObject*
mod_datasources(PyObject* self)
{
    UNUSED(self);
  
    if (henv == SQL_NULL_HANDLE && !AllocateEnv())
        return 0;

    PyObject* result = PyDict_New();
    if (!result)
        return 0;

    SQLCHAR szDSN[SQL_MAX_DSN_LENGTH];
    SWORD cbDSN;
    SQLCHAR szDesc[200];
    SWORD cbDesc;

    SQLUSMALLINT nDirection = SQL_FETCH_FIRST;

    SQLRETURN ret;

    for (;;)
    {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLDataSources(henv, SQL_FETCH_NEXT, szDSN,  _countof(szDSN),  &cbDSN, szDesc, _countof(szDesc), &cbDesc);
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret))
            break;
        
        PyDict_SetItemString(result, (const char*)szDSN, PyString_FromString((const char*)szDesc));
        nDirection = SQL_FETCH_NEXT;
    }
    
    if (ret != SQL_NO_DATA)
    {
        Py_DECREF(result);
        return RaiseErrorFromHandle("SQLDataSources", SQL_NULL_HANDLE, SQL_NULL_HANDLE);
    }
    
    return result;
}



static PyObject*
mod_timefromticks(PyObject* self, PyObject* args)
{
    UNUSED(self);
    
    time_t t = 0;
    struct tm* fields;

    // Sigh...  If a float is passed but we ask for a long, we get a deprecation warning printed to the screen instead
    // of a failure.  Not only is this not documented, it means we can't reliably use PyArg_ParseTuple('l') anywhere!
    
    // if (PyArg_ParseTuple(args, "l", &ticks))

    PyObject* num;
    if (!PyArg_ParseTuple(args, "O", &num))
        return 0;
    
    if (PyInt_Check(num))
        t = PyInt_AS_LONG(num);
    else if (PyLong_Check(num))
        t = PyLong_AsLong(num);
    else if (PyFloat_Check(num))
        t = (long)PyFloat_AS_DOUBLE(num);
    else
    {
        PyErr_SetString(PyExc_TypeError, "TimeFromTicks requires a number.");
        return 0;
    }
    
    fields = localtime(&t);

    return PyTime_FromTime(fields->tm_hour, fields->tm_min, fields->tm_sec, 0);
}

static PyObject*
mod_datefromticks(PyObject* self, PyObject* args)
{
    UNUSED(self);
    return PyDate_FromTimestamp(args);
}

static PyObject*
mod_timestampfromticks(PyObject* self, PyObject* args)
{
    UNUSED(self);
    return PyDateTime_FromTimestamp(args);
}

static char connect_doc[] =
    "connect(str, autocommit=False, ansi=False) --> Connection\n"            
    "\n"                                                                
    "Accepts an ODBC connection string and returns a new Connection object.\n" 
    "\n"                                                                
    "The connection string will be passed to SQLDriverConnect, so a DSN connection\n" 
    "can be created using:\n"                                           
    "\n"                                                                
    "  DSN=DataSourceName;UID=user;PWD=password\n"  
    "\n"                                                                
    "To connect without requiring a DSN, specify the driver and connection\n" 
    "information:\n"                                                    
    "\n"                                                                
    "  DRIVER={SQL Server};SERVER=localhost;DATABASE=testdb;UID=user;PWD=password\n" 
    "\n"                                                                
    "Note the use of braces when a value contains spaces.  Refer to SQLDriverConnect\n" 
    "documentation or the documentation of your ODBC driver for details.\n"
    "\n"
    "autocommit\n"
    "  If False or zero, the default, transactions are created automatically as defined in the \n"
    "  DB API 2.  If True or non-zero, the connection is put into ODBC autocommit mode and statements\n"
    "  are committed automatically.\n"
    "\n"
    "ansi\n"
    "  By default, pyodbc first attempts to connect using the Unicode version of SQLDriverConnectW.\n"
    "  If the driver returns IM001 indicating it does not support the Unicode version, the ANSI\n"
    "  version is tried.  Any other SQLSTATE is turned into an exception.  Setting ansi to true\n"
    "  skips the Unicode attempt and only connects using the ANSI version.  This is useful for\n"
    "  drivers that return the wrong SQLSTATE (or if pyodbc is out of date and should support\n"
    "  other SQLSTATEs).";
    
static char timefromticks_doc[] = 
    "TimeFromTicks(ticks) --> datetime.time\n"  \
    "\n"                                                                \
    "Returns a time object initialized from the given ticks value (number of seconds\n" \
    "since the epoch; see the documentation of the standard Python time module for\n" \
    "details).";

static char datefromticks_doc[] = 
    "DateFromTicks(ticks) --> datetime.date\n"  \
    "\n"                                                                \
    "Returns a date object initialized from the given ticks value (number of seconds\n" \
    "since the epoch; see the documentation of the standard Python time module for\n" \
    "details).";

static char timestampfromticks_doc[] = 
    "TimestampFromTicks(ticks) --> datetime.datetime\n"  \
    "\n"                                                                \
    "Returns a datetime object initialized from the given ticks value (number of\n" \
    "seconds since the epoch; see the documentation of the standard Python time\n" \
    "module for details";

static char datasources_doc[] =
    "dataSources() -> { DSN : Description }\n" \
    "\n" \
    "Returns a dictionary mapping available DSNs to their descriptions.";


static PyMethodDef pyodbc_methods[] =
{
    { "connect",            (PyCFunction)mod_connect,            METH_VARARGS|METH_KEYWORDS, connect_doc },
    { "TimeFromTicks",      (PyCFunction)mod_timefromticks,      METH_VARARGS,               timefromticks_doc },
    { "DateFromTicks",      (PyCFunction)mod_datefromticks,      METH_VARARGS,               datefromticks_doc },
    { "TimestampFromTicks", (PyCFunction)mod_timestampfromticks, METH_VARARGS,               timestampfromticks_doc },
    { "dataSources",        (PyCFunction)mod_datasources,        METH_NOARGS,                datasources_doc },
    { 0, 0, 0, 0 }
};


static void ErrorInit()
{
    // Called during startup to initialize any variables that will be freed by ErrorCleanup.

    Error = 0;
    Warning = 0;
    InterfaceError = 0;
    DatabaseError = 0;
    InternalError = 0;
    OperationalError = 0;
    ProgrammingError = 0;
    IntegrityError = 0;
    DataError = 0;
    NotSupportedError = 0;
    decimal_type = 0;
}


static void ErrorCleanup()
{
    // Called when an error occurs during initialization to release any objects we may have accessed.  Make sure each
    // item released was initialized to zero.  (Static objects are -- non-statics should be initialized in ErrorInit.)

    Py_XDECREF(Error);
    Py_XDECREF(Warning);
    Py_XDECREF(InterfaceError);
    Py_XDECREF(DatabaseError);
    Py_XDECREF(InternalError);
    Py_XDECREF(OperationalError);
    Py_XDECREF(ProgrammingError);
    Py_XDECREF(IntegrityError);
    Py_XDECREF(DataError);
    Py_XDECREF(NotSupportedError);
    Py_XDECREF(decimal_type);
}

struct ConstantDef
{
    const char* szName;
    int value;
};

#define MAKECONST(v) { #v, v }

static const ConstantDef aConstants[] = {
    MAKECONST(SQL_UNKNOWN_TYPE),
    MAKECONST(SQL_CHAR),
    MAKECONST(SQL_VARCHAR),
    MAKECONST(SQL_LONGVARCHAR),
    MAKECONST(SQL_WCHAR),
    MAKECONST(SQL_WVARCHAR),
    MAKECONST(SQL_WLONGVARCHAR),
    MAKECONST(SQL_DECIMAL),
    MAKECONST(SQL_NUMERIC),
    MAKECONST(SQL_SMALLINT),
    MAKECONST(SQL_INTEGER),
    MAKECONST(SQL_REAL),
    MAKECONST(SQL_FLOAT),
    MAKECONST(SQL_DOUBLE),
    MAKECONST(SQL_BIT),
    MAKECONST(SQL_TINYINT),
    MAKECONST(SQL_BIGINT),
    MAKECONST(SQL_BINARY),
    MAKECONST(SQL_VARBINARY),
    MAKECONST(SQL_LONGVARBINARY),
    MAKECONST(SQL_TYPE_DATE),
    MAKECONST(SQL_TYPE_TIME),
    MAKECONST(SQL_TYPE_TIMESTAMP),
    MAKECONST(SQL_SS_TIME2),
    MAKECONST(SQL_INTERVAL_MONTH),
    MAKECONST(SQL_INTERVAL_YEAR),
    MAKECONST(SQL_INTERVAL_YEAR_TO_MONTH),
    MAKECONST(SQL_INTERVAL_DAY),
    MAKECONST(SQL_INTERVAL_HOUR),
    MAKECONST(SQL_INTERVAL_MINUTE),
    MAKECONST(SQL_INTERVAL_SECOND),
    MAKECONST(SQL_INTERVAL_DAY_TO_HOUR),
    MAKECONST(SQL_INTERVAL_DAY_TO_MINUTE),
    MAKECONST(SQL_INTERVAL_DAY_TO_SECOND),
    MAKECONST(SQL_INTERVAL_HOUR_TO_MINUTE),
    MAKECONST(SQL_INTERVAL_HOUR_TO_SECOND),
    MAKECONST(SQL_INTERVAL_MINUTE_TO_SECOND),
    MAKECONST(SQL_GUID),
    MAKECONST(SQL_NULLABLE),
    MAKECONST(SQL_NO_NULLS),
    MAKECONST(SQL_NULLABLE_UNKNOWN),
    // MAKECONST(SQL_INDEX_BTREE),
    // MAKECONST(SQL_INDEX_CLUSTERED),
    // MAKECONST(SQL_INDEX_CONTENT),
    // MAKECONST(SQL_INDEX_HASHED),
    // MAKECONST(SQL_INDEX_OTHER),
    MAKECONST(SQL_SCOPE_CURROW),
    MAKECONST(SQL_SCOPE_TRANSACTION),
    MAKECONST(SQL_SCOPE_SESSION),
    MAKECONST(SQL_PC_UNKNOWN),
    MAKECONST(SQL_PC_NOT_PSEUDO),
    MAKECONST(SQL_PC_PSEUDO),

    // SQLGetInfo
    MAKECONST(SQL_ACCESSIBLE_PROCEDURES),
    MAKECONST(SQL_ACCESSIBLE_TABLES),
    MAKECONST(SQL_ACTIVE_ENVIRONMENTS),
    MAKECONST(SQL_AGGREGATE_FUNCTIONS),
    MAKECONST(SQL_ALTER_DOMAIN),
    MAKECONST(SQL_ALTER_TABLE),
    MAKECONST(SQL_ASYNC_MODE),
    MAKECONST(SQL_BATCH_ROW_COUNT),
    MAKECONST(SQL_BATCH_SUPPORT),
    MAKECONST(SQL_BOOKMARK_PERSISTENCE),
    MAKECONST(SQL_CATALOG_LOCATION),
    MAKECONST(SQL_CATALOG_NAME),
    MAKECONST(SQL_CATALOG_NAME_SEPARATOR),
    MAKECONST(SQL_CATALOG_TERM),
    MAKECONST(SQL_CATALOG_USAGE),
    MAKECONST(SQL_COLLATION_SEQ),
    MAKECONST(SQL_COLUMN_ALIAS),
    MAKECONST(SQL_CONCAT_NULL_BEHAVIOR),
    MAKECONST(SQL_CONVERT_FUNCTIONS),
    MAKECONST(SQL_CONVERT_VARCHAR),
    MAKECONST(SQL_CORRELATION_NAME),
    MAKECONST(SQL_CREATE_ASSERTION),
    MAKECONST(SQL_CREATE_CHARACTER_SET),
    MAKECONST(SQL_CREATE_COLLATION),
    MAKECONST(SQL_CREATE_DOMAIN),
    MAKECONST(SQL_CREATE_SCHEMA),
    MAKECONST(SQL_CREATE_TABLE),
    MAKECONST(SQL_CREATE_TRANSLATION),
    MAKECONST(SQL_CREATE_VIEW),
    MAKECONST(SQL_CURSOR_COMMIT_BEHAVIOR),
    MAKECONST(SQL_CURSOR_ROLLBACK_BEHAVIOR),
    // MAKECONST(SQL_CURSOR_ROLLBACK_SQL_CURSOR_SENSITIVITY),
    MAKECONST(SQL_DATABASE_NAME),
    MAKECONST(SQL_DATA_SOURCE_NAME),
    MAKECONST(SQL_DATA_SOURCE_READ_ONLY),
    MAKECONST(SQL_DATETIME_LITERALS),
    MAKECONST(SQL_DBMS_NAME),
    MAKECONST(SQL_DBMS_VER),
    MAKECONST(SQL_DDL_INDEX),
    MAKECONST(SQL_DEFAULT_TXN_ISOLATION),
    MAKECONST(SQL_DESCRIBE_PARAMETER),
    MAKECONST(SQL_DM_VER),
    MAKECONST(SQL_DRIVER_HDESC),
    MAKECONST(SQL_DRIVER_HENV),
    MAKECONST(SQL_DRIVER_HLIB),
    MAKECONST(SQL_DRIVER_HSTMT),
    MAKECONST(SQL_DRIVER_NAME),
    MAKECONST(SQL_DRIVER_ODBC_VER),
    MAKECONST(SQL_DRIVER_VER),
    MAKECONST(SQL_DROP_ASSERTION),
    MAKECONST(SQL_DROP_CHARACTER_SET),
    MAKECONST(SQL_DROP_COLLATION),
    MAKECONST(SQL_DROP_DOMAIN),
    MAKECONST(SQL_DROP_SCHEMA),
    MAKECONST(SQL_DROP_TABLE),
    MAKECONST(SQL_DROP_TRANSLATION),
    MAKECONST(SQL_DROP_VIEW),
    MAKECONST(SQL_DYNAMIC_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_DYNAMIC_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_EXPRESSIONS_IN_ORDERBY),
    MAKECONST(SQL_FILE_USAGE),
    MAKECONST(SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_GETDATA_EXTENSIONS),
    MAKECONST(SQL_GROUP_BY),
    MAKECONST(SQL_IDENTIFIER_CASE),
    MAKECONST(SQL_IDENTIFIER_QUOTE_CHAR),
    MAKECONST(SQL_INDEX_KEYWORDS),
    MAKECONST(SQL_INFO_SCHEMA_VIEWS),
    MAKECONST(SQL_INSERT_STATEMENT),
    MAKECONST(SQL_INTEGRITY),
    MAKECONST(SQL_KEYSET_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_KEYSET_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_KEYWORDS),
    MAKECONST(SQL_LIKE_ESCAPE_CLAUSE),
    MAKECONST(SQL_MAX_ASYNC_CONCURRENT_STATEMENTS),
    MAKECONST(SQL_MAX_BINARY_LITERAL_LEN),
    MAKECONST(SQL_MAX_CATALOG_NAME_LEN),
    MAKECONST(SQL_MAX_CHAR_LITERAL_LEN),
    MAKECONST(SQL_MAX_COLUMNS_IN_GROUP_BY),
    MAKECONST(SQL_MAX_COLUMNS_IN_INDEX),
    MAKECONST(SQL_MAX_COLUMNS_IN_ORDER_BY),
    MAKECONST(SQL_MAX_COLUMNS_IN_SELECT),
    MAKECONST(SQL_MAX_COLUMNS_IN_TABLE),
    MAKECONST(SQL_MAX_COLUMN_NAME_LEN),
    MAKECONST(SQL_MAX_CONCURRENT_ACTIVITIES),
    MAKECONST(SQL_MAX_CURSOR_NAME_LEN),
    MAKECONST(SQL_MAX_DRIVER_CONNECTIONS),
    MAKECONST(SQL_MAX_IDENTIFIER_LEN),
    MAKECONST(SQL_MAX_INDEX_SIZE),
    MAKECONST(SQL_MAX_PROCEDURE_NAME_LEN),
    MAKECONST(SQL_MAX_ROW_SIZE),
    MAKECONST(SQL_MAX_ROW_SIZE_INCLUDES_LONG),
    MAKECONST(SQL_MAX_SCHEMA_NAME_LEN),
    MAKECONST(SQL_MAX_STATEMENT_LEN),
    MAKECONST(SQL_MAX_TABLES_IN_SELECT),
    MAKECONST(SQL_MAX_TABLE_NAME_LEN),
    MAKECONST(SQL_MAX_USER_NAME_LEN),
    MAKECONST(SQL_MULTIPLE_ACTIVE_TXN),
    MAKECONST(SQL_MULT_RESULT_SETS),
    MAKECONST(SQL_NEED_LONG_DATA_LEN),
    MAKECONST(SQL_NON_NULLABLE_COLUMNS),
    MAKECONST(SQL_NULL_COLLATION),
    MAKECONST(SQL_NUMERIC_FUNCTIONS),
    MAKECONST(SQL_ODBC_INTERFACE_CONFORMANCE),
    MAKECONST(SQL_ODBC_VER),
    MAKECONST(SQL_OJ_CAPABILITIES),
    MAKECONST(SQL_ORDER_BY_COLUMNS_IN_SELECT),
    MAKECONST(SQL_PARAM_ARRAY_ROW_COUNTS),
    MAKECONST(SQL_PARAM_ARRAY_SELECTS),
    MAKECONST(SQL_PARAM_TYPE_UNKNOWN),
    MAKECONST(SQL_PARAM_INPUT),
    MAKECONST(SQL_PARAM_INPUT_OUTPUT),
    MAKECONST(SQL_PARAM_OUTPUT),
    MAKECONST(SQL_RETURN_VALUE),
    MAKECONST(SQL_RESULT_COL),
    MAKECONST(SQL_PROCEDURES),
    MAKECONST(SQL_PROCEDURE_TERM),
    MAKECONST(SQL_QUOTED_IDENTIFIER_CASE),
    MAKECONST(SQL_ROW_UPDATES),
    MAKECONST(SQL_SCHEMA_TERM),
    MAKECONST(SQL_SCHEMA_USAGE),
    MAKECONST(SQL_SCROLL_OPTIONS),
    MAKECONST(SQL_SEARCH_PATTERN_ESCAPE),
    MAKECONST(SQL_SERVER_NAME),
    MAKECONST(SQL_SPECIAL_CHARACTERS),
    MAKECONST(SQL_SQL92_DATETIME_FUNCTIONS),
    MAKECONST(SQL_SQL92_FOREIGN_KEY_DELETE_RULE),
    MAKECONST(SQL_SQL92_FOREIGN_KEY_UPDATE_RULE),
    MAKECONST(SQL_SQL92_GRANT),
    MAKECONST(SQL_SQL92_NUMERIC_VALUE_FUNCTIONS),
    MAKECONST(SQL_SQL92_PREDICATES),
    MAKECONST(SQL_SQL92_RELATIONAL_JOIN_OPERATORS),
    MAKECONST(SQL_SQL92_REVOKE),
    MAKECONST(SQL_SQL92_ROW_VALUE_CONSTRUCTOR),
    MAKECONST(SQL_SQL92_STRING_FUNCTIONS),
    MAKECONST(SQL_SQL92_VALUE_EXPRESSIONS),
    MAKECONST(SQL_SQL_CONFORMANCE),
    MAKECONST(SQL_STANDARD_CLI_CONFORMANCE),
    MAKECONST(SQL_STATIC_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_STATIC_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_STRING_FUNCTIONS),
    MAKECONST(SQL_SUBQUERIES),
    MAKECONST(SQL_SYSTEM_FUNCTIONS),
    MAKECONST(SQL_TABLE_TERM),
    MAKECONST(SQL_TIMEDATE_ADD_INTERVALS),
    MAKECONST(SQL_TIMEDATE_DIFF_INTERVALS),
    MAKECONST(SQL_TIMEDATE_FUNCTIONS),
    MAKECONST(SQL_TXN_CAPABLE),
    MAKECONST(SQL_TXN_ISOLATION_OPTION),
    MAKECONST(SQL_UNION),
    MAKECONST(SQL_USER_NAME),
    MAKECONST(SQL_XOPEN_CLI_YEAR),
};


static bool CreateExceptions()
{
    for (unsigned int i = 0; i < _countof(aExcInfos); i++)
    {
        ExcInfo& info = aExcInfos[i];

        PyObject* classdict = PyDict_New();
        if (!classdict)
            return false;

        PyObject* doc = PyString_FromString(info.szDoc);
        if (!doc)
        {
            Py_DECREF(classdict);
            return false;
        }
        
        PyDict_SetItemString(classdict, "__doc__", doc);
        Py_DECREF(doc);

        *info.ppexc = PyErr_NewException((char*)info.szFullName, *info.ppexcParent, classdict);
        if (*info.ppexc == 0)
        {
            Py_DECREF(classdict);
            return false;
        }
        
        // Keep a reference for our internal (C++) use.
        Py_INCREF(*info.ppexc);

        PyModule_AddObject(pModule, (char*)info.szName, *info.ppexc);
    }

    return true;
}


PyMODINIT_FUNC
initpyodbc()
{
#ifdef _DEBUG
    #ifndef Py_REF_DEBUG
    #error Py_REF_DEBUG not set!
    #endif

    int grfDebugFlags = _CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF;
    _CrtSetDbgFlag(grfDebugFlags);
#endif

    ErrorInit();

    if (PyType_Ready(&ConnectionType) < 0 || PyType_Ready(&CursorType) < 0 || PyType_Ready(&RowType) < 0 || PyType_Ready(&CnxnInfoType) < 0)
        return;

    pModule = Py_InitModule4("pyodbc", pyodbc_methods, module_doc, NULL, PYTHON_API_VERSION);

    if (!import_types())
        return;

    init_locale_info();

    if (!CreateExceptions())
        return;

    // The 'build' version number is a beta identifier.  For example, if it is 7, then we are on beta7 of the
    // (major,minor.micro) version.  On Windows, we poke these values into the DLL's version resource, so when we make
    // an official build (which come *after* the betas), we set the BUILD to 9999 so installers will know that it
    // should replace any installed betas.  However, we obviously don't want to see these.

    PyObject* pVersion;
    if (PYODBC_BUILD == 9999)
        pVersion = PyString_FromFormat("%d.%d.%d", PYODBC_MAJOR, PYODBC_MINOR, PYODBC_MICRO);
    else
        pVersion = PyString_FromFormat("%d.%d.%d-beta%d", PYODBC_MAJOR, PYODBC_MINOR, PYODBC_MICRO, PYODBC_BUILD);
    PyModule_AddObject(pModule, "version", pVersion);

    PyModule_AddIntConstant(pModule, "threadsafety", 1);
    PyModule_AddStringConstant(pModule, "apilevel", "2.0");
    PyModule_AddStringConstant(pModule, "paramstyle", "qmark");
    PyModule_AddObject(pModule, "pooling", Py_True);
    Py_INCREF(Py_True);
    PyModule_AddObject(pModule, "lowercase", Py_False);
    Py_INCREF(Py_False);
                       
    PyModule_AddObject(pModule, "Connection", (PyObject*)&ConnectionType);
    Py_INCREF((PyObject*)&ConnectionType);
    PyModule_AddObject(pModule, "Cursor", (PyObject*)&CursorType);
    Py_INCREF((PyObject*)&CursorType);
    PyModule_AddObject(pModule, "Row", (PyObject*)&RowType);
    Py_INCREF((PyObject*)&RowType);

    // Add the SQL_XXX defines from ODBC.
    for (unsigned int i = 0; i < _countof(aConstants); i++)
        PyModule_AddIntConstant(pModule, (char*)aConstants[i].szName, aConstants[i].value);

    PyModule_AddObject(pModule, "Date", (PyObject*)PyDateTimeAPI->DateType);
    Py_INCREF((PyObject*)PyDateTimeAPI->DateType);
    PyModule_AddObject(pModule, "Time", (PyObject*)PyDateTimeAPI->TimeType);
    Py_INCREF((PyObject*)PyDateTimeAPI->TimeType);
    PyModule_AddObject(pModule, "Timestamp", (PyObject*)PyDateTimeAPI->DateTimeType);
    Py_INCREF((PyObject*)PyDateTimeAPI->DateTimeType);
    PyModule_AddObject(pModule, "DATETIME", (PyObject*)PyDateTimeAPI->DateTimeType);
    Py_INCREF((PyObject*)PyDateTimeAPI->DateTimeType);
    PyModule_AddObject(pModule, "STRING", (PyObject*)&PyString_Type);
    Py_INCREF((PyObject*)&PyString_Type);
    PyModule_AddObject(pModule, "NUMBER", (PyObject*)&PyFloat_Type);
    Py_INCREF((PyObject*)&PyFloat_Type);
    PyModule_AddObject(pModule, "ROWID", (PyObject*)&PyInt_Type);
    Py_INCREF((PyObject*)&PyInt_Type);
    PyModule_AddObject(pModule, "BINARY", (PyObject*)&PyBuffer_Type);
    Py_INCREF((PyObject*)&PyBuffer_Type);
    PyModule_AddObject(pModule, "Binary", (PyObject*)&PyBuffer_Type);
    Py_INCREF((PyObject*)&PyBuffer_Type);
    
    if (PyErr_Occurred())
        ErrorCleanup();
}

#ifdef WINVER
BOOL WINAPI DllMain(
  HINSTANCE hMod,
  DWORD fdwReason,
  LPVOID lpvReserved
  )
{
    UNUSED(hMod, fdwReason, lpvReserved);
    return TRUE;
}
#endif
