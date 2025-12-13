# jsonpointer - PostgreSQL data type for JsonPointer

This extension implements a **PostgreSQL** type for storing data in
[RFC-6901](https://datatracker.ietf.org/doc/html/rfc6901)
format and using it to fetch attributes or subdocuments
from **jsonb** data.

## Installation

### From Source Code

Make sure the `pg_config` utility of the system's PostgreSQL insallation
is in `$PATH`.

Clone the git repository

```
git clone https://github.com/wieck/jsonpointer.git
```

Build the binaires

```
cd jsonpointer
make
```

Install the binaries

```
sudo PATH=$PATH make install
```

**Note:** sudo is needed because when PostgreSQL is installed from
RPMs the binaries are installed as **root**. This means installing
and extension requires **root** privileges. If your system was
installed differently (like you did an unprivileged user installation
of **PostgreSQL**, then you will know what else to do to install
an extension in your environment.

## Installing the Extension inside a Database

`jsonpointer` is installed like every standard **PostgreSQL Extension**.

```
CREATE EXTENSION jsonpointer [SCHEMA <schemaname>];
```

It is installed by default in the `public` schema because data types and
related functions are normally used across an entire database and
installing them in a specific schema is rather inconvenient.

## Using the Data Type

The `jsonpointer` data type itself is rudimentary at this point. It
just consists of the type specific input and output functions and the
typedeclaration. There currently doesn't seem to be a need for any
more complicated actions like comparators or index support.

### Storing JsonPointer Data in a Table

`jsonpointer` can be used like any other data type in `CREATE TABLE`:

```
CREATE TABLE tab_jsonptr (
    id  bigserial PRIMARY KEY,
    ptr jsonpointer NOT NULL
);
```

The `jsonpointer` data type parses the input string and converts it
into an internal, binary, on-disk representation. Only a string that
follows the rules of
[RFC-6901](https://datatracker.ietf.org/doc/html/rfc6901)
is accepted. This ensures that downstream systems using the JsonPointer
cannot encounter invalid data.

### Functions to Fetch Attributes

All functions returning attributes follow a simple pattern. Their name
indicates the **PostgreSQL** data type they are attempting to return.
The function

```
jsonpointer_get_int8(jsonb, jsonpointer, nullonerror=true)
```

tries to cast the scalar value identified by `jsonpointer` into an
`int8` value. It will return NULL if the attribute cannot be found or
if the string representation of the value cannot be cast into `int8`
and `nullonerror` is true. It will raise a **PostgreSQL** ERROR if
the string representation of the value cannot be cast into `int8` and
`nullonerror` is false.

The currently implemented functions are

* `jsonpointer_get_jsonb()`
* `jsonpointer_get_text()`
* `jsonpointer_get_int4()`
* `jsonpointer_get_int8()`
* `jsonpointer_get_numeric()`
* `jsonpointer_get_timestamptz()`
