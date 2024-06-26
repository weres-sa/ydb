======
pg8000
======

.. |ssl.SSLContext| replace:: ``ssl.SSLContext``
.. _ssl.SSLContext: https://docs.python.org/3/library/ssl.html#ssl.SSLContext

.. |ssl.create_default_context()| replace:: ``ssl.create_default_context()``
.. _ssl.create_default_context(): https://docs.python.org/3/library/ssl.html#ssl.create_default_context

pg8000 is a pure-`Python <https://www.python.org/>`_
`PostgreSQL <http://www.postgresql.org/>`_ driver that complies with
`DB-API 2.0 <http://www.python.org/dev/peps/pep-0249/>`_. It is tested on Python
versions 3.8+, on CPython and PyPy, and PostgreSQL versions 12+. pg8000's name comes
from the belief that it is probably about the 8000th PostgreSQL interface for Python.
pg8000 is distributed under the BSD 3-clause license.

All bug reports, feature requests and contributions are welcome at
`http://github.com/tlocke/pg8000/ <http://github.com/tlocke/pg8000/>`_.

.. image:: https://github.com/tlocke/pg8000/workflows/pg8000/badge.svg
   :alt: Build Status

.. contents:: Table of Contents
   :depth: 2
   :local:

Installation
------------

To install pg8000 using `pip` type:

`pip install pg8000`


Native API Interactive Examples
-------------------------------

pg8000 comes with two APIs, the native pg8000 API and the DB-API 2.0 standard
API. These are the examples for the native API, and the DB-API 2.0 examples
follow in the next section.


Basic Example
`````````````

Import pg8000, connect to the database, create a table, add some rows and then
query the table:

>>> import pg8000.native
>>>
>>> # Connect to the database with user name postgres
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> # Create a temporary table
>>>
>>> con.run("CREATE TEMPORARY TABLE book (id SERIAL, title TEXT)")
>>>
>>> # Populate the table
>>>
>>> for title in ("Ender's Game", "The Magus"):
...     con.run("INSERT INTO book (title) VALUES (:title)", title=title)
>>>
>>> # Print all the rows in the table
>>>
>>> for row in con.run("SELECT * FROM book"):
...     print(row)
[1, "Ender's Game"]
[2, 'The Magus']
>>>
>>> con.close()


Transactions
````````````

Here's how to run groups of SQL statements in a
`transaction <https://www.postgresql.org/docs/current/tutorial-transactions.html>`_:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("START TRANSACTION")
>>>
>>> # Create a temporary table
>>> con.run("CREATE TEMPORARY TABLE book (id SERIAL, title TEXT)")
>>>
>>> for title in ("Ender's Game", "The Magus", "Phineas Finn"):
...     con.run("INSERT INTO book (title) VALUES (:title)", title=title)
>>> con.run("COMMIT")
>>> for row in con.run("SELECT * FROM book"):
...     print(row)
[1, "Ender's Game"]
[2, 'The Magus']
[3, 'Phineas Finn']
>>>
>>> con.close()

rolling back a transaction:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> # Create a temporary table
>>> con.run("CREATE TEMPORARY TABLE book (id SERIAL, title TEXT)")
>>>
>>> for title in ("Ender's Game", "The Magus", "Phineas Finn"):
...     con.run("INSERT INTO book (title) VALUES (:title)", title=title)
>>>
>>> con.run("START TRANSACTION")
>>> con.run("DELETE FROM book WHERE title = :title", title="Phineas Finn") 
>>> con.run("ROLLBACK")
>>> for row in con.run("SELECT * FROM book"):
...     print(row)
[1, "Ender's Game"]
[2, 'The Magus']
[3, 'Phineas Finn']
>>>
>>> con.close()

NB. There is `a longstanding bug <https://github.com/tlocke/pg8000/issues/36>`_
in the PostgreSQL server whereby if a `COMMIT` is issued against a failed
transaction, the transaction is silently rolled back, rather than an error being
returned. pg8000 attempts to detect when this has happened and raise an
`InterfaceError`.


Query Using Functions
`````````````````````

Another query, using some PostgreSQL functions:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT TO_CHAR(TIMESTAMP '2021-10-10', 'YYYY BC')")
[['2021 AD']]
>>>
>>> con.close()


Interval Type
`````````````

A query that returns the PostgreSQL interval type:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> import datetime
>>>
>>> ts = datetime.date(1980, 4, 27)
>>> con.run("SELECT timestamp '2013-12-01 16:06' - :ts", ts=ts)
[[datetime.timedelta(days=12271, seconds=57960)]]
>>>
>>> con.close()


Point Type
``````````

A round-trip with a
`PostgreSQL point <https://www.postgresql.org/docs/current/datatype-geometric.html>`_
type:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT CAST(:pt as point)", pt=(2.3,1))
[[(2.3, 1.0)]]
>>>
>>> con.close()


Client Encoding
```````````````

When communicating with the server, pg8000 uses the character set that the server asks
it to use (the client encoding). By default the client encoding is the database's
character set (chosen when the database is created), but the client encoding can be
changed in a number of ways (eg. setting ``CLIENT_ENCODING`` in ``postgresql.conf``).
Another way of changing the client encoding is by using an SQL command. For example:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SET CLIENT_ENCODING TO 'UTF8'")
>>> con.run("SHOW CLIENT_ENCODING")
[['UTF8']]
>>>
>>> con.close()


JSON
````

`JSON <https://www.postgresql.org/docs/current/datatype-json.html>`_ always comes back
from the server de-serialized. If the JSON you want to send is a ``dict`` then you can
just do:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> val = {'name': 'Apollo 11 Cave', 'zebra': True, 'age': 26.003}
>>> con.run("SELECT CAST(:apollo as jsonb)", apollo=val)
[[{'age': 26.003, 'name': 'Apollo 11 Cave', 'zebra': True}]]
>>>
>>> con.close()

JSON can always be sent in serialized form to the server:

>>> import json
>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>>
>>> val = ['Apollo 11 Cave', True, 26.003]
>>> con.run("SELECT CAST(:apollo as jsonb)", apollo=json.dumps(val))
[[['Apollo 11 Cave', True, 26.003]]]
>>>
>>> con.close()

JSON queries can be have parameters:

>>> import pg8000.native
>>>
>>> with pg8000.native.Connection("postgres", password="cpsnow") as con:
...     con.run(""" SELECT CAST('{"a":1, "b":2}' AS jsonb) @> :v """, v={"b": 2})
[[True]]


Retrieve Column Metadata From Results
`````````````````````````````````````

Find the column metadata returned from a query:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("create temporary table quark (id serial, name text)")
>>> for name in ('Up', 'Down'):
...     con.run("INSERT INTO quark (name) VALUES (:name)", name=name)
>>> # Now execute the query
>>>
>>> con.run("SELECT * FROM quark")
[[1, 'Up'], [2, 'Down']]
>>>
>>> # and retrieve the metadata
>>>
>>> con.columns
[{'table_oid': ..., 'column_attrnum': 1, 'type_oid': 23, 'type_size': 4, 'type_modifier': -1, 'format': 0, 'name': 'id'}, {'table_oid': ..., 'column_attrnum': 2, 'type_oid': 25, 'type_size': -1, 'type_modifier': -1, 'format': 0, 'name': 'name'}]
>>>
>>> # Show just the column names
>>>
>>> [c['name'] for c in con.columns]
['id', 'name']
>>>
>>> con.close()


Notices And Notifications
`````````````````````````

PostgreSQL `notices
<https://www.postgresql.org/docs/current/static/plpgsql-errors-and-messages.html>`_ are
stored in a deque called ``Connection.notices`` and added using the ``append()``
method. Similarly there are ``Connection.notifications`` for `notifications
<https://www.postgresql.org/docs/current/static/sql-notify.html>`_. Here's an example:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("LISTEN aliens_landed")
>>> con.run("NOTIFY aliens_landed")
>>> # A notification is a tuple containing (backend_pid, channel, payload)
>>>
>>> con.notifications[0]
(..., 'aliens_landed', '')
>>>
>>> con.close()


Parameter Statuses
``````````````````

`Certain parameter values are reported by the server automatically at connection startup or whenever
their values change
<https://www.postgresql.org/docs/current/libpq-status.html#LIBPQ-PQPARAMETERSTATUS>`_ and pg8000
stores the latest values in a dict called ``Connection.parameter_statuses``. Here's an example where
we set the ``aplication_name`` parameter and then read it from the ``parameter_statuses``:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection(
...     "postgres", password="cpsnow", application_name='AGI')
>>>
>>> con.parameter_statuses['application_name']
'AGI'
>>>
>>> con.close()


LIMIT ALL
`````````

You might think that the following would work, but in fact it fails:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT 'silo 1' LIMIT :lim", lim='ALL')
Traceback (most recent call last):
pg8000.exceptions.DatabaseError: ...
>>>
>>> con.close()

Instead the `docs say <https://www.postgresql.org/docs/current/sql-select.html>`_ that
you can send ``null`` as an alternative to ``ALL``, which does work:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT 'silo 1' LIMIT :lim", lim=None)
[['silo 1']]
>>>
>>> con.close()


IN and NOT IN
`````````````

You might think that the following would work, but in fact the server doesn't like it:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT 'silo 1' WHERE 'a' IN :v", v=['a', 'b'])
Traceback (most recent call last):
pg8000.exceptions.DatabaseError: ...
>>>
>>> con.close()

instead you can write it using the `unnest
<https://www.postgresql.org/docs/current/functions-array.html>`_ function:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run(
...     "SELECT 'silo 1' WHERE 'a' IN (SELECT unnest(CAST(:v as varchar[])))",
...     v=['a', 'b'])
[['silo 1']]
>>> con.close()

and you can do the same for ``NOT IN``.


Many SQL Statements Can't Be Parameterized
``````````````````````````````````````````

In PostgreSQL parameters can only be used for `data values, not identifiers
<https://www.postgresql.org/docs/current/xfunc-sql.html#XFUNC-SQL-FUNCTION-ARGUMENTS>`_.
Sometimes this might not work as expected, for example the following fails:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> channel = 'top_secret'
>>>
>>> con.run("LISTEN :channel", channel=channel)
Traceback (most recent call last):
pg8000.exceptions.DatabaseError: ...
>>>
>>> con.close()

It fails because the PostgreSQL server doesn't allow this statement to have any
parameters. There are many SQL statements that one might think would have parameters,
but don't. For these cases the SQL has to be created manually, being careful to use the
``identifier()`` and ``literal()`` functions to escape the values to avoid `SQL
injection attacks <https://en.wikipedia.org/wiki/SQL_injection>`_:

>>> from pg8000.native import Connection, identifier, literal
>>>
>>> con = Connection("postgres", password="cpsnow")
>>>
>>> channel = 'top_secret'
>>> payload = 'Aliens Landed!'
>>> con.run(f"LISTEN {identifier(channel)}")
>>> con.run(f"NOTIFY {identifier(channel)}, {literal(payload)}")
>>>
>>> con.notifications[0]
(..., 'top_secret', 'Aliens Landed!')
>>>
>>> con.close()


COPY FROM And TO A Stream
`````````````````````````

The SQL `COPY <https://www.postgresql.org/docs/current/sql-copy.html>`_ statement can be
used to copy from and to a file or file-like object. Here' an example using the CSV
format:

>>> import pg8000.native
>>> from io import StringIO
>>> import csv
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> # Create a CSV file in memory
>>>
>>> stream_in = StringIO()
>>> csv_writer = csv.writer(stream_in)
>>> csv_writer.writerow([1, "electron"])
12
>>> csv_writer.writerow([2, "muon"])
8
>>> csv_writer.writerow([3, "tau"])
7
>>> stream_in.seek(0)
0
>>>
>>> # Create a table and then copy the CSV into it
>>>
>>> con.run("CREATE TEMPORARY TABLE lepton (id SERIAL, name TEXT)")
>>> con.run("COPY lepton FROM STDIN WITH (FORMAT CSV)", stream=stream_in)
>>>
>>> # COPY from a table to a stream
>>>
>>> stream_out = StringIO()
>>> con.run("COPY lepton TO STDOUT WITH (FORMAT CSV)", stream=stream_out)
>>> stream_out.seek(0)
0
>>> for row in csv.reader(stream_out):
...     print(row)
['1', 'electron']
['2', 'muon']
['3', 'tau']
>>>
>>> con.close()

It's also possible to COPY FROM an iterable, which is useful if you're creating rows
programmatically:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> # Generator function for creating rows
>>> def row_gen():
...     for i, name in ((1, "electron"), (2, "muon"), (3, "tau")):
...         yield f"{i},{name}\n"
>>>
>>> # Create a table and then copy the CSV into it
>>>
>>> con.run("CREATE TEMPORARY TABLE lepton (id SERIAL, name TEXT)")
>>> con.run("COPY lepton FROM STDIN WITH (FORMAT CSV)", stream=row_gen())
>>>
>>> # COPY from a table to a stream
>>>
>>> stream_out = StringIO()
>>> con.run("COPY lepton TO STDOUT WITH (FORMAT CSV)", stream=stream_out)
>>> stream_out.seek(0)
0
>>> for row in csv.reader(stream_out):
...     print(row)
['1', 'electron']
['2', 'muon']
['3', 'tau']
>>>
>>> con.close()


Execute Multiple SQL Statements
```````````````````````````````

If you want to execute a series of SQL statements (eg. an ``.sql`` file), you can run
them as expected:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> statements = "SELECT 5; SELECT 'Erich Fromm';"
>>>
>>> con.run(statements)
[[5], ['Erich Fromm']]
>>>
>>> con.close()

The only caveat is that when executing multiple statements you can't have any
parameters.


Quoted Identifiers in SQL
`````````````````````````

Say you had a column called ``My Column``. Since it's case sensitive and contains a
space, you'd have to `surround it by double quotes
<https://www.postgresql.org/docs/current/sql-syntax-lexical.html#SQL-SYNTAX-IDENTIFIER>`_.
But you can't do:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("select 'hello' as "My Column"")
Traceback (most recent call last):
SyntaxError: invalid syntax...
>>>
>>> con.close()

since Python uses double quotes to delimit string literals, so one solution is
to use Python's `triple quotes
<https://docs.python.org/3/tutorial/introduction.html#strings>`_ to delimit the string
instead:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run('''SELECT 'hello' AS "My Column"''')
[['hello']]
>>>
>>> con.close()

another solution, that's especially useful if the identifier comes from an untrusted
source, is to use the ``identifier()`` function, which correctly quotes and escapes the
identifier as needed:

>>> from pg8000.native import Connection, identifier
>>>
>>> con = Connection("postgres", password="cpsnow")
>>>
>>> sql = f"SELECT 'hello' as {identifier('My Column')}"
>>> print(sql)
SELECT 'hello' as "My Column"
>>>
>>> con.run(sql)
[['hello']]
>>>
>>> con.close()

this approach guards against `SQL injection attacks
<https://en.wikipedia.org/wiki/SQL_injection>`_. One thing to note if you're using
explicit schemas (eg. ``pg_catalog.pg_language``) is that the schema name and table name
are both separate identifiers. So to escape them you'd do:

>>> from pg8000.native import Connection, identifier
>>>
>>> con = Connection("postgres", password="cpsnow")
>>>
>>> query = (
...     f"SELECT lanname FROM {identifier('pg_catalog')}.{identifier('pg_language')} "
...     f"WHERE lanname = 'sql'"
... )
>>> print(query)
SELECT lanname FROM pg_catalog.pg_language WHERE lanname = 'sql'
>>>
>>> con.run(query)
[['sql']]
>>>
>>> con.close()


Custom adapter from a Python type to a PostgreSQL type
``````````````````````````````````````````````````````

pg8000 has a mapping from Python types to PostgreSQL types for when it needs to send
SQL parameters to the server. The default mapping that comes with pg8000 is designed to
work well in most cases, but you might want to add or replace the default mapping.

A Python ``datetime.timedelta`` object is sent to the server as a PostgreSQL
``interval`` type,  which has the ``oid`` 1186. But let's say we wanted to create our
own Python class to be sent as an ``interval`` type. Then we'd have to register an
adapter:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> class MyInterval(str):
...     pass
>>>
>>> def my_interval_out(my_interval):
...     return my_interval  # Must return a str
>>>
>>> con.register_out_adapter(MyInterval, my_interval_out)
>>> con.run("SELECT CAST(:interval as interval)", interval=MyInterval("2 hours"))
[[datetime.timedelta(seconds=7200)]]
>>>
>>> con.close()

Note that it still came back as a ``datetime.timedelta`` object because we only changed
the mapping from Python to PostgreSQL. See below for an example of how to change the
mapping from PostgreSQL to Python.


Custom adapter from a PostgreSQL type to a Python type
``````````````````````````````````````````````````````

pg8000 has a mapping from PostgreSQL types to Python types for when it receives SQL
results from the server. The default mapping that comes with pg8000 is designed to work
well in most cases, but you might want to add or replace the default mapping.

If pg8000 receives PostgreSQL ``interval`` type, which has the ``oid`` 1186, it converts
it into a Python ``datetime.timedelta`` object. But let's say we wanted to create our
own Python class to be used instead of ``datetime.timedelta``. Then we'd have to
register an adapter:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> class MyInterval(str):
...     pass
>>>
>>> def my_interval_in(my_interval_str):  # The parameter is of type str
...     return MyInterval(my_interval)
>>>
>>> con.register_in_adapter(1186, my_interval_in)
>>> con.run("SELECT \'2 years'")
[['2 years']]
>>>
>>> con.close()

Note that registering the 'in' adapter only afects the mapping from the PostgreSQL type
to the Python type. See above for an example of how to change the mapping from
PostgreSQL to Python.


Could Not Determine Data Type Of Parameter
``````````````````````````````````````````

Sometimes you'll get the 'could not determine data type of parameter' error message from
the server:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT :v IS NULL", v=None)
Traceback (most recent call last):
pg8000.exceptions.DatabaseError: {'S': 'ERROR', 'V': 'ERROR', 'C': '42P18', 'M': 'could not determine data type of parameter $1', 'F': 'postgres.c', 'L': '...', 'R': '...'}
>>>
>>> con.close()

One way of solving it is to put a ``CAST`` in the SQL:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT cast(:v as TIMESTAMP) IS NULL", v=None)
[[True]]
>>>
>>> con.close()

Another way is to override the type that pg8000 sends along with each parameter:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> con.run("SELECT :v IS NULL", v=None, types={'v': pg8000.native.TIMESTAMP})
[[True]]
>>>
>>> con.close()


Prepared Statements
```````````````````

`Prepared statements <https://www.postgresql.org/docs/current/sql-prepare.html>`_
can be useful in improving performance when you have a statement that's executed
repeatedly. Here's an example:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection("postgres", password="cpsnow")
>>>
>>> # Create the prepared statement
>>> ps = con.prepare("SELECT cast(:v as varchar)")
>>>
>>> # Execute the statement repeatedly
>>> ps.run(v="speedy")
[['speedy']]
>>> ps.run(v="rapid")
[['rapid']]
>>> ps.run(v="swift")
[['swift']]
>>>
>>> # Close the prepared statement, releasing resources on the server
>>> ps.close()
>>>
>>> con.close()


Use Environment Variables As Connection Defaults
````````````````````````````````````````````````

You might want to use the current user as the database username for example:

>>> import pg8000.native
>>> import getpass
>>>
>>> # Connect to the database with current user name
>>> username = getpass.getuser()
>>> connection = pg8000.native.Connection(username, password="cpsnow")
>>>
>>> connection.run("SELECT 'pilau'")
[['pilau']]
>>>
>>> connection.close()

or perhaps you may want to use some of the same `environment variables that libpg uses
<https://www.postgresql.org/docs/current/libpq-envars.html>`_:

>>> import pg8000.native
>>> from os import environ
>>>
>>> username = environ.get('PGUSER', 'postgres')
>>> password = environ.get('PGPASSWORD', 'cpsnow')
>>> host = environ.get('PGHOST', 'localhost')
>>> port = environ.get('PGPORT', '5432')
>>> database = environ.get('PGDATABASE')
>>>
>>> connection = pg8000.native.Connection(
...     username, password=password, host=host, port=port, database=database)
>>>
>>> connection.run("SELECT 'Mr Cairo'")
[['Mr Cairo']]
>>>
>>> connection.close()

It might be asked, why doesn't pg8000 have this behaviour built in? The thinking
follows the second aphorism of `The Zen of Python
<https://www.python.org/dev/peps/pep-0020/>`_:

    Explicit is better than implicit.

So we've taken the approach of only being able to set connection parameters using the
``pg8000.native.Connection()`` constructor.


Connect To PostgreSQL Over SSL
``````````````````````````````

To connect to the server using SSL defaults do::

  import pg8000.native
  connection = pg8000.native.Connection('postgres', password="cpsnow", ssl_context=True)
  connection.run("SELECT 'The game is afoot!'")

To connect over SSL with custom settings, set the ``ssl_context`` parameter to an
|ssl.SSLContext|_ object:

::

  import pg8000.native
  import ssl


  ssl_context = ssl.create_default_context()
  ssl_context.verify_mode = ssl.CERT_REQUIRED
  ssl_context.load_verify_locations('root.pem')        
  connection = pg8000.native.Connection(
    'postgres', password="cpsnow", ssl_context=ssl_context)

It may be that your PostgreSQL server is behind an SSL proxy server in which case you
can set a pg8000-specific attribute ``ssl.SSLContext.request_ssl = False`` which tells
pg8000 to connect using an SSL socket, but not to request SSL from the PostgreSQL
server:

::

  import pg8000.native
  import ssl

  ssl_context = ssl.create_default_context()
  ssl_context.request_ssl = False
  connection = pg8000.native.Connection(
      'postgres', password="cpsnow", ssl_context=ssl_context)


Server-Side Cursors
```````````````````

You can use the SQL commands `DECLARE
<https://www.postgresql.org/docs/current/sql-declare.html>`_,
`FETCH <https://www.postgresql.org/docs/current/sql-fetch.html>`_,
`MOVE <https://www.postgresql.org/docs/current/sql-move.html>`_ and
`CLOSE <https://www.postgresql.org/docs/current/sql-close.html>`_ to manipulate
server-side cursors. For example:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection('postgres', password="cpsnow")
>>> con.run("START TRANSACTION")
>>> con.run("DECLARE c SCROLL CURSOR FOR SELECT * FROM generate_series(1, 100)")
>>> con.run("FETCH FORWARD 5 FROM c")
[[1], [2], [3], [4], [5]]
>>> con.run("MOVE FORWARD 50 FROM c")
>>> con.run("FETCH BACKWARD 10 FROM c")
[[54], [53], [52], [51], [50], [49], [48], [47], [46], [45]]
>>> con.run("CLOSE c")
>>> con.run("ROLLBACK")
>>>
>>> con.close()


BLOBs (Binary Large Objects)
````````````````````````````

There's a set of `SQL functions
<https://www.postgresql.org/docs/current/lo-funcs.html>`_ for manipulating BLOBs.
Here's an example:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection('postgres', password="cpsnow")
>>>
>>> # Create a BLOB and get its oid
>>> data = b'hello'
>>> res = con.run("SELECT lo_from_bytea(0, :data)", data=data)
>>> oid = res[0][0]
>>>
>>> # Create a table and store the oid of the BLOB
>>> con.run("CREATE TEMPORARY TABLE image (raster oid)")
>>>
>>> con.run("INSERT INTO image (raster) VALUES (:oid)", oid=oid)
>>> # Retrieve the data using the oid
>>> con.run("SELECT lo_get(:oid)", oid=oid)
[[b'hello']]
>>>
>>> # Add some data to the end of the BLOB
>>> more_data = b' all'
>>> offset = len(data)
>>> con.run(
...     "SELECT lo_put(:oid, :offset, :data)",
...     oid=oid, offset=offset, data=more_data)
[['']]
>>> con.run("SELECT lo_get(:oid)", oid=oid)
[[b'hello all']]
>>>
>>> # Download a part of the data
>>> con.run("SELECT lo_get(:oid, 6, 3)", oid=oid)
[[b'all']]
>>>
>>> con.close()


Replication Protocol
````````````````````

The PostgreSQL `Replication Protocol
<https://www.postgresql.org/docs/current/protocol-replication.html>`_ is supported using
the ``replication`` keyword when creating a connection:

>>> import pg8000.native
>>>
>>> con = pg8000.native.Connection(
...    'postgres', password="cpsnow", replication="database")
>>>
>>> con.run("IDENTIFY_SYSTEM")
[['...', 1, '.../...', 'postgres']]
>>>
>>> con.close()


DB-API 2 Interactive Examples
-----------------------------

These examples stick to the DB-API 2.0 standard.


Basic Example
`````````````

Import pg8000, connect to the database, create a table, add some rows and then query the
table:

>>> import pg8000.dbapi
>>>
>>> conn = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cursor = conn.cursor()
>>> cursor.execute("CREATE TEMPORARY TABLE book (id SERIAL, title TEXT)")
>>> cursor.execute(
...     "INSERT INTO book (title) VALUES (%s), (%s) RETURNING id, title",
...     ("Ender's Game", "Speaker for the Dead"))
>>> results = cursor.fetchall()
>>> for row in results:
...     id, title = row
...     print("id = %s, title = %s" % (id, title))
id = 1, title = Ender's Game
id = 2, title = Speaker for the Dead
>>> conn.commit()
>>>
>>> conn.close()


Query Using Functions
`````````````````````

Another query, using some PostgreSQL functions:

>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cursor = con.cursor()
>>>
>>> cursor.execute("SELECT TO_CHAR(TIMESTAMP '2021-10-10', 'YYYY BC')")
>>> cursor.fetchone()
['2021 AD']
>>>
>>> con.close()


Interval Type
`````````````

A query that returns the PostgreSQL interval type:

>>> import datetime
>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cursor = con.cursor()
>>>
>>> cursor.execute("SELECT timestamp '2013-12-01 16:06' - %s",
... (datetime.date(1980, 4, 27),))
>>> cursor.fetchone()
[datetime.timedelta(days=12271, seconds=57960)]
>>>
>>> con.close()


Point Type
``````````

A round-trip with a `PostgreSQL point
<https://www.postgresql.org/docs/current/datatype-geometric.html>`_ type:

>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cursor = con.cursor()
>>>
>>> cursor.execute("SELECT cast(%s as point)", ((2.3,1),))
>>> cursor.fetchone()
[(2.3, 1.0)]
>>>
>>> con.close()


Numeric Parameter Style
```````````````````````

pg8000 supports all the DB-API parameter styles. Here's an example of using the
'numeric' parameter style:

>>> import pg8000.dbapi
>>>
>>> pg8000.dbapi.paramstyle = "numeric"
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cursor = con.cursor()
>>>
>>> cursor.execute("SELECT array_prepend(:1, CAST(:2 AS int[]))", (500, [1, 2, 3, 4],))
>>> cursor.fetchone()
[[500, 1, 2, 3, 4]]
>>> pg8000.dbapi.paramstyle = "format"
>>>
>>> con.close()


Autocommit
``````````

Following the DB-API specification, autocommit is off by default. It can be turned on by
using the autocommit property of the connection:

>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> con.autocommit = True
>>>
>>> cur = con.cursor()
>>> cur.execute("vacuum")
>>> conn.autocommit = False
>>> cur.close()
>>>
>>> con.close()


Client Encoding
```````````````

When communicating with the server, pg8000 uses the character set that the server asks
it to use (the client encoding). By default the client encoding is the database's
character set (chosen when the database is created), but the client encoding can be
changed in a number of ways (eg. setting ``CLIENT_ENCODING`` in ``postgresql.conf``).
Another way of changing the client encoding is by using an SQL command. For example:

>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cur = con.cursor()
>>> cur.execute("SET CLIENT_ENCODING TO 'UTF8'")
>>> cur.execute("SHOW CLIENT_ENCODING")
>>> cur.fetchone()
['UTF8']
>>> cur.close()
>>>
>>> con.close()


JSON
````

JSON is sent to the server serialized, and returned de-serialized. Here's an example:

>>> import json
>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cur = con.cursor()
>>> val = ['Apollo 11 Cave', True, 26.003]
>>> cur.execute("SELECT cast(%s as json)", (json.dumps(val),))
>>> cur.fetchone()
[['Apollo 11 Cave', True, 26.003]]
>>> cur.close()
>>>
>>> con.close()

JSON queries can be have parameters:

>>> import pg8000.dbapi
>>>
>>> with pg8000.dbapi.connect("postgres", password="cpsnow") as con:
...     cur = con.cursor()
...     cur.execute(""" SELECT CAST('{"a":1, "b":2}' AS jsonb) @> %s """, ({"b": 2},))
...     for row in cur.fetchall():
...         print(row)
[True]


Retrieve Column Names From Results
``````````````````````````````````

Use the columns names retrieved from a query:

>>> import pg8000
>>> conn = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> c = conn.cursor()
>>> c.execute("create temporary table quark (id serial, name text)")
>>> c.executemany("INSERT INTO quark (name) VALUES (%s)", (("Up",), ("Down",)))
>>> #
>>> # Now retrieve the results
>>> #
>>> c.execute("select * from quark")
>>> rows = c.fetchall()
>>> keys = [k[0] for k in c.description]
>>> results = [dict(zip(keys, row)) for row in rows]
>>> assert results == [{'id': 1, 'name': 'Up'}, {'id': 2, 'name': 'Down'}]
>>>
>>> conn.close()


COPY from and to a file
```````````````````````

The SQL `COPY <https://www.postgresql.org/docs/current/sql-copy.html>`__ statement can
be used to copy from and to a file or file-like object:

>>> from io import StringIO
>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cur = con.cursor()
>>> #
>>> # COPY from a stream to a table
>>> #
>>> stream_in = StringIO('1\telectron\n2\tmuon\n3\ttau\n')
>>> cur = con.cursor()
>>> cur.execute("create temporary table lepton (id serial, name text)")
>>> cur.execute("COPY lepton FROM stdin", stream=stream_in)
>>> #
>>> # Now COPY from a table to a stream
>>> #
>>> stream_out = StringIO()
>>> cur.execute("copy lepton to stdout", stream=stream_out)
>>> stream_out.getvalue()
'1\telectron\n2\tmuon\n3\ttau\n'
>>>
>>> con.close()


Server-Side Cursors
```````````````````

You can use the SQL commands `DECLARE
<https://www.postgresql.org/docs/current/sql-declare.html>`_,
`FETCH <https://www.postgresql.org/docs/current/sql-fetch.html>`_,
`MOVE <https://www.postgresql.org/docs/current/sql-move.html>`_ and
`CLOSE <https://www.postgresql.org/docs/current/sql-close.html>`_ to manipulate
server-side cursors. For example:

>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cur = con.cursor()
>>> cur.execute("START TRANSACTION")
>>> cur.execute(
...    "DECLARE c SCROLL CURSOR FOR SELECT * FROM generate_series(1, 100)")
>>> cur.execute("FETCH FORWARD 5 FROM c")
>>> cur.fetchall()
([1], [2], [3], [4], [5])
>>> cur.execute("MOVE FORWARD 50 FROM c")
>>> cur.execute("FETCH BACKWARD 10 FROM c")
>>> cur.fetchall()
([54], [53], [52], [51], [50], [49], [48], [47], [46], [45])
>>> cur.execute("CLOSE c")
>>> cur.execute("ROLLBACK")
>>>
>>> con.close()


BLOBs (Binary Large Objects)
````````````````````````````

There's a set of `SQL functions
<https://www.postgresql.org/docs/current/lo-funcs.html>`_ for manipulating BLOBs.
Here's an example:

>>> import pg8000.dbapi
>>>
>>> con = pg8000.dbapi.connect(user="postgres", password="cpsnow")
>>> cur = con.cursor()
>>>
>>> # Create a BLOB and get its oid
>>> data = b'hello'
>>> cur = con.cursor()
>>> cur.execute("SELECT lo_from_bytea(0, %s)", [data])
>>> oid = cur.fetchone()[0]
>>>
>>> # Create a table and store the oid of the BLOB
>>> cur.execute("CREATE TEMPORARY TABLE image (raster oid)")
>>> cur.execute("INSERT INTO image (raster) VALUES (%s)", [oid])
>>>
>>> # Retrieve the data using the oid
>>> cur.execute("SELECT lo_get(%s)", [oid])
>>> cur.fetchall()
([b'hello'],)
>>>
>>> # Add some data to the end of the BLOB
>>> more_data = b' all'
>>> offset = len(data)
>>> cur.execute("SELECT lo_put(%s, %s, %s)", [oid, offset, more_data])
>>> cur.execute("SELECT lo_get(%s)", [oid])
>>> cur.fetchall()
([b'hello all'],)
>>>
>>> # Download a part of the data
>>> cur.execute("SELECT lo_get(%s, 6, 3)", [oid])
>>> cur.fetchall()
([b'all'],)
>>>
>>> con.close()


Type Mapping
------------

The following table shows the default mapping between Python types and PostgreSQL types,
and vice versa.

If pg8000 doesn't recognize a type that it receives from PostgreSQL, it will return it
as a ``str`` type. This is how pg8000 handles PostgreSQL ``enum`` and XML types. It's
possible to change the default mapping using adapters (see the examples).

.. table:: Python to PostgreSQL Type Mapping

   +-----------------------+-----------------+-----------------------------------------+
   | Python Type           | PostgreSQL Type | Notes                                   |
   +=======================+=================+=========================================+
   | bool                  | bool            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | int                   | int4            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | str                   | text            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | float                 | float8          |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | decimal.Decimal       | numeric         |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | bytes                 | bytea           |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | datetime.datetime     | timestamp       | +/-infinity PostgreSQL values are       |
   | (without tzinfo)      | without         | represented as Python ``str`` values.   |
   |                       | timezone        | If a ``timestamp`` is too big for       |
   |                       |                 | ``datetime.datetime`` then a ``str`` is |
   |                       |                 | used.                                   |
   +-----------------------+-----------------+-----------------------------------------+
   | datetime.datetime     | timestamp with  | +/-infinity PostgreSQL values are       |
   | (with tzinfo)         | timezone        | represented as Python ``str`` values.   |
   |                       |                 | If a ``timestamptz`` is too big for     |
   |                       |                 | ``datetime.datetime`` then a ``str`` is |
   |                       |                 | used.                                   |
   +-----------------------+-----------------+-----------------------------------------+
   | datetime.date         | date            | +/-infinity PostgreSQL values are       |
   |                       |                 | represented as Python ``str`` values.   |
   |                       |                 | If a ``date`` is too big for a          |
   |                       |                 | ``datetime.date`` then a ``str`` is     |
   |                       |                 | used.                                   |
   +-----------------------+-----------------+-----------------------------------------+
   | datetime.time         | time without    |                                         |
   |                       | time zone       |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | datetime.timedelta    | interval        | If an ``interval`` is too big for       |
   |                       |                 | ``datetime.timedelta`` then a           |
   |                       |                 | ``PGInterval``  is used.                |
   +-----------------------+-----------------+-----------------------------------------+
   | None                  | NULL            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | uuid.UUID             | uuid            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | ipaddress.IPv4Address | inet            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | ipaddress.IPv6Address | inet            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | ipaddress.IPv4Network | inet            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | ipaddress.IPv6Network | inet            |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | int                   | xid             |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | list of int           | INT4[]          |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | list of float         | FLOAT8[]        |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | list of bool          | BOOL[]          |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | list of str           | TEXT[]          |                                         |
   +-----------------------+-----------------+-----------------------------------------+
   | int                   | int2vector      | Only from PostgreSQL to Python          |
   +-----------------------+-----------------+-----------------------------------------+
   | JSON                  | json, jsonb     | The Python JSON is provided as a Python |
   |                       |                 | serialized string. Results returned as  |
   |                       |                 | de-serialized JSON.                     |
   +-----------------------+-----------------+-----------------------------------------+
   | pg8000.Range          | \*range         | PostgreSQL multirange types are         |
   |                       |                 | represented in Python as a list of      |
   |                       |                 | range types.                            |
   +-----------------------+-----------------+-----------------------------------------+
   | tuple                 | composite type  | Only from Python to PostgreSQL          |
   +-----------------------+-----------------+-----------------------------------------+



Theory Of Operation
-------------------

  A concept is tolerated inside the microkernel only if moving it outside the kernel,
  i.e., permitting competing implementations, would prevent the implementation of the
  system's required functionality.

  -- Jochen Liedtke, Liedtke's minimality principle

pg8000 is designed to be used with one thread per connection.

Pg8000 communicates with the database using the `PostgreSQL Frontend/Backend Protocol
<https://www.postgresql.org/docs/current/protocol.html>`_ (FEBE). If a query has no
parameters, pg8000 uses the 'simple query protocol'. If a query does have parameters,
pg8000 uses the 'extended query protocol' with unnamed prepared statements. The steps
for a query with parameters are:

1. Query comes in.

#. Send a PARSE message to the server to create an unnamed prepared statement.

#. Send a BIND message to run against the unnamed prepared statement, resulting in an
   unnamed portal on the server.

#. Send an EXECUTE message to read all the results from the portal.

It's also possible to use named prepared statements. In which case the prepared
statement persists on the server, and represented in pg8000 using a
``PreparedStatement`` object. This means that the PARSE step gets executed once up
front, and then only the BIND and EXECUTE steps are repeated subsequently.

There are a lot of PostgreSQL data types, but few primitive data types in Python. By
default, pg8000 doesn't send PostgreSQL data type information in the PARSE step, in
which case PostgreSQL assumes the types implied by the SQL statement. In some cases
PostgreSQL can't work out a parameter type and so an `explicit cast
<https://www.postgresql.org/docs/current/static/sql-expressions.html#SQL-SYNTAX-TYPE-CASTS>`_
can be used in the SQL.

In the FEBE protocol, each query parameter can be sent to the server either as binary
or text according to the format code. In pg8000 the parameters are always sent as text.

Occasionally, the network connection between pg8000 and the server may go down. If
pg8000 encounters a network problem it'll raise an ``InterfaceError`` with the message
``network error`` and with the original exception set as the `cause
<https://docs.python.org/3/reference/simple_stmts.html#the-raise-statement>`_.


Native API Docs
---------------

`Native API Docs <docs/native_api_docs.rst>`_


DB-API 2 Docs
-------------

`DB-API 2 Docs <docs/dbapi2_docs.rst>`_


Design Decisions
----------------

For the ``Range`` type, the constructor follows the `PostgreSQL range constructor functions <https://www.postgresql.org/docs/current/rangetypes.html#RANGETYPES-CONSTRUCT>`_
which makes `[closed, open) <https://fhur.me/posts/always-use-closed-open-intervals>`_
the easiest to express:

>>> from pg8000.types import Range
>>>
>>> pg_range = Range(2, 6)


Tests
-----

- Install `tox <http://testrun.org/tox/latest/>`_: ``pip install tox``

- Enable the PostgreSQL hstore extension by running the SQL command:
  ``create extension hstore;``

- Add a line to ``pg_hba.conf`` for the various authentication options:

::

  host    pg8000_md5           all        127.0.0.1/32            md5
  host    pg8000_gss           all        127.0.0.1/32            gss
  host    pg8000_password      all        127.0.0.1/32            password
  host    pg8000_scram_sha_256 all        127.0.0.1/32            scram-sha-256
  host    all                  all        127.0.0.1/32            trust

- Set password encryption to ``scram-sha-256`` in ``postgresql.conf``:
  ``password_encryption = 'scram-sha-256'``

- Set the password for the postgres user: ``ALTER USER postgresql WITH PASSWORD 'pw';``

- Run ``tox`` from the ``pg8000`` directory: ``tox``

This will run the tests against the Python version of the virtual environment, on the
machine, and the installed PostgreSQL version listening on port 5432, or the ``PGPORT``
environment variable if set.

Benchmarks are run as part of the test suite at ``tests/test_benchmarks.py``.


README.rst
----------

This file is written in the `reStructuredText
<https://docutils.sourceforge.io/docs/user/rst/quickref.html>`_ format. To generate an
HTML page from it, do:

- Activate the virtual environment: ``source venv/bin/activate``
- Install ``Sphinx``: ``pip install Sphinx``
- Run ``rst2html.py``: ``rst2html.py README.rst README.html``


Doing A Release Of pg8000
-------------------------

Run ``tox`` to make sure all tests pass, then update the release notes, then do:

::

  git tag -a x.y.z -m "version x.y.z"
  rm -r dist
  python -m build
  twine upload dist/*


Release Notes
-------------

`Release Notes <docs/release_notes.rst>`_
