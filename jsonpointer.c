/* -------------------------------------------------------------------------
 *
 * jsonpointer.c
 *
 * 		PostgreSQL data type implementing RFC6901 JSON pointer
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "jsonpointer.h"

#include "fmgr.h"
#include "varatt.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(jsonpointer_in);
PG_FUNCTION_INFO_V1(jsonpointer_out);

PG_FUNCTION_INFO_V1(jsonptr_get_jsonb);
PG_FUNCTION_INFO_V1(jsonptr_get_text);
PG_FUNCTION_INFO_V1(jsonptr_get_int4);
PG_FUNCTION_INFO_V1(jsonptr_get_int8);
PG_FUNCTION_INFO_V1(jsonptr_get_numeric);
PG_FUNCTION_INFO_V1(jsonptr_get_timestamptz);

/* Internal parser states */
#define JPTR_PARSE_STATE_INIT		0
#define JPTR_PARSE_STATE_ELEM		1
#define JPTR_PARSE_STATE_ESCAPE		2

static Datum jsonptr_get_jsonb_datum(Jsonb *jb, JsonPointer *jsonptr,
									 bool *isnull, bool as_text);
static Datum jsonptr_cast_datum1(Datum value, PGFunction func, bool *isnull,
								 bool nullonerror);
static Datum jsonptr_cast_datum3(Datum value, PGFunction func, bool *isnull,
								 bool nullonerror, Datum arg2, Datum arg3);


/*
 * jsonpointer_in()
 *
 * 	jsonpointer data type input function
 */
Datum
jsonpointer_in(PG_FUNCTION_ARGS)
{
	char				   *input = PG_GETARG_CSTRING(0);
	StringInfoData			result;
	JsonPointer			   *jsonptr;
	text				   *elem = NULL;
	int						elem_start = 0;
	int						i;
	int						state = JPTR_PARSE_STATE_INIT;
	char				   *cp = input;

	/*
	 * Create the result in a StringInfo and initialize it to hold the
	 * JsonPointer header with zero elements.
	 */
	initStringInfo(&result);
	jsonptr = (JsonPointer *)(result.data);
	for (i = 0; i < sizeof(JsonPointer); i++)
		appendStringInfoChar(&result, 0);

	/*
	 * parse the input C-string
	 *
	 * Note that neither '~' (0xfe) nor '/' (0x2f) can ever occur in the
	 * continuation byte of a UTF-8 sequence. We therefore do not need to
	 * track if we are inside of a UTF-8 multibyte sequence or not.
	 */
	while (*cp)
	{
		int32	len_aligned;

		switch (state)
		{
			case JPTR_PARSE_STATE_INIT:
				/*
				 * the very first character of a non-empty JsonPointer
				 * must be '/'
				 */
				if (*cp++ != '/')
					elog(ERROR, "invalid JsonPointer '%s'", input);

				/*
				 * transition into element mode and remember where the
				 * text element starts, advance for the 4 vl_len_ bytes.
				 */
				state = JPTR_PARSE_STATE_ELEM;

				elem = (text *)(result.data + result.len);
				elem_start = result.len;
				for (i = 0; i < VARHDRSZ; i++)
					appendStringInfoChar(&result, 0);
				break;

			case JPTR_PARSE_STATE_ELEM:
				/* we are inside the path name of an element */
				switch (*cp)
				{
					case '/':
						/*
						 * this is a plain '/'.
						 *
						 * this marks the end of the previous path element
						 * and the beginning of the next. Set the VARSIZE
						 * of this element and count it.
						 */
						cp++;
						SET_VARSIZE(elem, result.len - elem_start);
						jsonptr->n_elem++;

						/*
						 * INTALING() for the next elem so it starts at
						 * a 4-byte boundary in the result.
						 */
						len_aligned = INTALIGN(result.len);
						while (result.len < len_aligned)
							appendStringInfoChar(&result, 0);

						/* Start the new element and add the vl_len_ */
						elem = (text *)(result.data + result.len);
						elem_start = result.len;
						for (i = 0; i < VARHDRSZ; i++)
							appendStringInfoChar(&result, 0);

						break;

					case '~':
						/* this is the start of a ~0 or ~1 escape sequence */
						cp++;
						state = JPTR_PARSE_STATE_ESCAPE;
						break;

					default:
						/* just a regular path character, add and move on */
						appendStringInfoChar(&result, *cp++);
						break;
				}
				break;

			case JPTR_PARSE_STATE_ESCAPE:
				/*
				 * the previous character was the '~' escape. At this point
				 * we just skip a following '0' or '1' but error out early
				 * so we don't get into trouble later.
				 */
				switch (*cp++)
				{
					case '0':
						appendStringInfoChar(&result, '~');
						state = JPTR_PARSE_STATE_ELEM;
						break;

					case '1':
						appendStringInfoChar(&result, '/');
						state = JPTR_PARSE_STATE_ELEM;
						break;

					default:
						elog(ERROR, "invalid JsonPointer escape sequence");
						break;
				}
				break;

			default:
				/* invalid parse state is a serious bug */
				elog(ERROR, "invalid JsonPointer state during parse");
				break;
		}
	}

	/*
	 * We reached the end of the input, collect the eventually open last path
	 * element or report an error if this happened during an escape sequence.
	 */
	switch (state)
	{
		case JPTR_PARSE_STATE_ELEM:
			/* still inside of a path element, set its VARSIZE and count it */
			SET_VARSIZE(elem, result.len - elem_start);
			jsonptr->n_elem++;
			break;

		case JPTR_PARSE_STATE_ESCAPE:
			/* a lone tilde should not be at the end */
			elog(ERROR, "invalid JsonPointer escape sequence");
			break;

		default:
			break;
	}

	/* All done - set the varlena header and return the result */
	SET_VARSIZE(result.data, result.len);
	PG_RETURN_POINTER(result.data);
}

/*
 * jsonpointer_out()
 *
 * 	jsonpointer data type output function
 */
Datum
jsonpointer_out(PG_FUNCTION_ARGS)
{
	JsonPointer	   *jsonptr;
	text		   *elem;
	StringInfoData	result;
	int				i;

	/* make sure we have a detoasted, plain jsonpointer object to work on */
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	elem = (text *)((char *)(jsonptr) + sizeof(JsonPointer));

	/* initialize the output C-string result buffer */
	initStringInfo(&result);

	/* process all elements in the jsonpointer */
	for (i = 0; i < jsonptr->n_elem; i++)
	{
		char   *cp;
		int32	elem_len;

		/*
		 * for each of the path elements we add a '/' and the string
		 * representation of their key, encoding '~' and '/' with the
		 * proper '~0' and '~1'.
		 */
		appendStringInfoChar(&result, '/');
		cp = (char *)VARDATA(elem);
		for (elem_len = VARSIZE(elem) - VARHDRSZ; elem_len > 0; elem_len--)
		{
			if (*cp == '~')
			{
				appendStringInfoChar(&result, '~');
				appendStringInfoChar(&result, '0');
				cp++;
			}
			else if (*cp == '/')
			{
				appendStringInfoChar(&result, '~');
				appendStringInfoChar(&result, '1');
				cp++;
			}
			else
			{
				appendStringInfoChar(&result, *cp++);
			}
		}

		/* advance elem to the next properly aligned text */
		elem = (text *)INTALIGN((char *)elem + VARSIZE(elem));
	}

	/* nothing else to do */
	PG_RETURN_CSTRING(result.data);
}

/*
 * jsonptr_get_jsonb()
 *
 * 	SQL callable function to retrieve a jsonb version of the attribute
 * 	specified by JsonPointer. This could be any subelement, not just a
 * 	scalar value.
 */
Datum
jsonptr_get_jsonb(PG_FUNCTION_ARGS)
{
	Jsonb		   *jb;
	JsonPointer	   *jsonptr;
	Datum			result;
	bool			isnull;

	/* nullonerror is ignored here because any jsonb element is jsonb */

	jb = PG_GETARG_JSONB_P(0);
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(1));

	result = jsonptr_get_jsonb_datum(jb, jsonptr, &isnull, false);
	if (isnull)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(result);
}

/*
 * jsonptr_get_text()
 *
 * 	SQL callable function to retrieve a text version of the attribute
 * 	specified by JsonPointer.
 */
Datum
jsonptr_get_text(PG_FUNCTION_ARGS)
{
	Jsonb		   *jb;
	JsonPointer	   *jsonptr;
	Datum			result;
	bool			isnull;

	/* nullonerror is ignored here because anything can be returned as text */

	jb = PG_GETARG_JSONB_P(0);
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(1));

	result = jsonptr_get_jsonb_datum(jb, jsonptr, &isnull, true);
	if (isnull)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(result);
}

/*
 * jsonptr_get_int4()
 *
 * 	SQL callable function to retrieve a int4 version of the attribute
 * 	specified by JsonPointer.
 */
Datum
jsonptr_get_int4(PG_FUNCTION_ARGS)
{
	Jsonb		   *jb;
	JsonPointer	   *jsonptr;
	Datum			result;
	bool			isnull;
	bool			nullonerror;

	jb = PG_GETARG_JSONB_P(0);
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	nullonerror = PG_GETARG_BOOL(2);

	result = jsonptr_get_jsonb_datum(jb, jsonptr, &isnull, true);
	if (isnull)
		PG_RETURN_NULL();

	result = jsonptr_cast_datum1(result, int4in, &isnull, nullonerror);
	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

/*
 * jsonptr_get_int8()
 *
 * 	SQL callable function to retrieve a int8 version of the attribute
 * 	specified by JsonPointer.
 */
Datum
jsonptr_get_int8(PG_FUNCTION_ARGS)
{
	Jsonb		   *jb;
	JsonPointer	   *jsonptr;
	Datum			result;
	bool			isnull;
	bool			nullonerror;

	jb = PG_GETARG_JSONB_P(0);
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	nullonerror = PG_GETARG_BOOL(2);

	result = jsonptr_get_jsonb_datum(jb, jsonptr, &isnull, true);
	if (isnull)
		PG_RETURN_NULL();

	result = jsonptr_cast_datum1(result, int8in, &isnull, nullonerror);
	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

/*
 * jsonptr_get_numeric()
 *
 * 	SQL callable function to retrieve a numeric version of the attribute
 * 	specified by JsonPointer.
 */
Datum
jsonptr_get_numeric(PG_FUNCTION_ARGS)
{
	Jsonb		   *jb;
	JsonPointer	   *jsonptr;
	Datum			result;
	bool			isnull;
	bool			nullonerror;

	jb = PG_GETARG_JSONB_P(0);
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	nullonerror = PG_GETARG_BOOL(2);

	result = jsonptr_get_jsonb_datum(jb, jsonptr, &isnull, true);
	if (isnull)
		PG_RETURN_NULL();

	result = jsonptr_cast_datum3(result, numeric_in, &isnull, nullonerror,
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(0));
	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

/*
 * jsonptr_get_timestamptz()
 *
 * 	SQL callable function to retrieve a timestamptz version of the attribute
 * 	specified by JsonPointer.
 */
Datum
jsonptr_get_timestamptz(PG_FUNCTION_ARGS)
{
	Jsonb		   *jb;
	JsonPointer	   *jsonptr;
	Datum			result;
	bool			isnull;
	bool			nullonerror;

	jb = PG_GETARG_JSONB_P(0);
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	nullonerror = PG_GETARG_BOOL(2);

	result = jsonptr_get_jsonb_datum(jb, jsonptr, &isnull, true);
	if (isnull)
		PG_RETURN_NULL();

	result = jsonptr_cast_datum3(result, timestamptz_in, &isnull, nullonerror,
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(0));
	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

/*
 * jsonptr_get_jsonb_datum()
 *
 * 	Convert our JsonPointer into an array of text Datums and use
 * 	jsonb_get_element() to find that value. Return it as a jsonb
 * 	or text datum as requested. The caller is responsible for
 * 	casting it to other data types.
 */
static Datum
jsonptr_get_jsonb_datum(Jsonb *jb, JsonPointer *jsonptr, bool *isnull,
						bool as_text)
{
	Datum			   *path;
	text			   *elem;
	int					i;

	elem = (text *)((char *)jsonptr + sizeof(JsonPointer));

	/* Convert all the JsonPointer elements into text Datums */
	path = palloc(sizeof(Datum) * jsonptr->n_elem);
	for (i = 0; i < jsonptr->n_elem; i++)
	{
		path[i] = PointerGetDatum(elem);
		elem = (text *)INTALIGN((char *)elem + VARSIZE(elem));
	}

	/* Let jsonb_get_element() do the actual work */
	return jsonb_get_element(jb, path, jsonptr->n_elem, isnull, as_text);
}

/*
 * jsonptr_cast_datum1()
 *
 *	Internal function to cast a text Datum into any other type via its
 *	type input function. We use a TRY/CATCH block and cast to NULL if
 *	asked to. This will prevent ERRORs when trying to use the jsonptr_get*
 *	functions in index expressions when a jsonpointer expression does
 *	not return a string that can be casted into that data type.
 */
static Datum
jsonptr_cast_datum1(Datum value, PGFunction func, bool *isnull,
				   bool nullonerror)
{
	if (nullonerror)
	{
		PG_TRY();
		{
			char *s = DatumGetCString(DirectFunctionCall1(textout, value));
			return DirectFunctionCall1(func, CStringGetDatum(s));
		}
		PG_CATCH();
		{
			*isnull = true;
			return 0;
		}
		PG_END_TRY();
	}
	else
	{
		char *s = DatumGetCString(DirectFunctionCall1(textout, value));
		return DirectFunctionCall1(func, CStringGetDatum(s));
	}
}

/*
 * jsonptr_cast_datum3()
 *
 * 	Cast function like above but for data types that take more input
 * 	arguments (like numeric and timestamptz).
 */
static Datum
jsonptr_cast_datum3(Datum value, PGFunction func, bool *isnull,
					bool nullonerror, Datum arg2, Datum arg3)
{
	if (nullonerror)
	{
		PG_TRY();
		{
			char *s = DatumGetCString(DirectFunctionCall1(textout, value));
			return DirectFunctionCall3(func, CStringGetDatum(s), arg2, arg3);
		}
		PG_CATCH();
		{
			*isnull = true;
			return 0;
		}
		PG_END_TRY();
	}
	else
	{
		char *s = DatumGetCString(DirectFunctionCall1(textout, value));
		return DirectFunctionCall3(func, CStringGetDatum(s), arg2, arg3);
	}
}
