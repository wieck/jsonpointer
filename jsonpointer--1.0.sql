/* jsonpointer--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonpointer" to load this file. \quit

CREATE FUNCTION jsonpointer_in(cstring)
	RETURNS jsonpointer
	AS '$libdir/jsonpointer', 'jsonpointer_in'
	LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION jsonpointer_out(jsonpointer)
	RETURNS cstring
	AS '$libdir/jsonpointer', 'jsonpointer_out'
	LANGUAGE C STRICT IMMUTABLE;

CREATE TYPE jsonpointer (
	INPUT = jsonpointer_in,
	OUTPUT = jsonpointer_out,
	INTERNALLENGTH = VARIABLE);

CREATE FUNCTION jsonptr_get_text(jsonb, jsonpointer,
								 nullonerror bool = false)
	RETURNS text
	AS '$libdir/jsonpointer', 'jsonptr_get_text'
	LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION jsonptr_get_int4(jsonb, jsonpointer,
								 nullonerror bool = false)
	RETURNS int4
	AS '$libdir/jsonpointer', 'jsonptr_get_int4'
	LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION jsonptr_get_int8(jsonb, jsonpointer,
								 nullonerror bool = false)
	RETURNS int8
	AS '$libdir/jsonpointer', 'jsonptr_get_int8'
	LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION jsonptr_get_numeric(jsonb, jsonpointer,
									nullonerror bool = false)
	RETURNS numeric
	AS '$libdir/jsonpointer', 'jsonptr_get_numeric'
	LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION jsonptr_get_timestamptz(jsonb, jsonpointer,
										nullonerror bool = false)
	RETURNS timestamptz
	AS '$libdir/jsonpointer', 'jsonptr_get_timestamptz'
	LANGUAGE C STRICT IMMUTABLE;

