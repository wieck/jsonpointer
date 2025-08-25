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

PG_FUNCTION_INFO_V1(jsonptr_get_text);
PG_FUNCTION_INFO_V1(jsonptr_get_int4);
PG_FUNCTION_INFO_V1(jsonptr_get_int8);
PG_FUNCTION_INFO_V1(jsonptr_get_numeric);
PG_FUNCTION_INFO_V1(jsonptr_get_timestamptz);

/* Internal parser states */
#define JPTR_PARSE_STATE_INIT		0
#define JPTR_PARSE_STATE_ELEM		1
#define JPTR_PARSE_STATE_ESCAPE		2

static inline void jsonpointer_collect_elem(StringInfo result, int32 keyStart);
static Datum jsonptr_get_text_datum(Jsonb *jb, JsonPointer *jsonptr,
									bool *isnull);
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
	int						i;
	int						state = JPTR_PARSE_STATE_INIT;
	char				   *cp = input;
	char				   *keyStart;

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
		switch (state)
		{
			case JPTR_PARSE_STATE_INIT:
				/* the very first character of a JsonPointer must be '/' */
				if (*cp++ != '/')
					elog(ERROR, "invalid JsonPointer '%s'", input);

				/*
				 * transition into element mode and remember where in the
				 * input cstring this key started
				 */
				keyStart = cp;
				state = JPTR_PARSE_STATE_ELEM;
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
						 * and the beginning of the next. Collect this one
						 * and start the next.
						 */
						jsonpointer_collect_elem(&result, keyStart - input);
						cp++;
						keyStart = cp;
						break;

					case '~':
						/* this is the start of a ~0 or ~1 escape sequence */
						cp++;
						state = JPTR_PARSE_STATE_ESCAPE;
						break;

					default:
						/* just a regular path character, add and move on */
						cp++;
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
					case '1':
						state = JPTR_PARSE_STATE_ELEM;
						break;

					default:
						elog(ERROR, "invalid JsonPointer escape sequence");
						break;
				}
				break;

			default:
				/* this would be a serious bug */
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
			/* still inside of a path element, collect it */
			jsonpointer_collect_elem(&result, keyStart - input);
			break;

		case JPTR_PARSE_STATE_ESCAPE:
			/* a lone tilde should not be at the end */
			elog(ERROR, "invalid JsonPointer escape sequence");
			break;

		default:
			break;
	}

	/*
	 * Now we finish the elements by converting their offsets, adding their
	 * actual string data and attempting to convert them into integer values.
	 */
	for (i = 0; i < jsonptr->n_elem; i++)
	{
		JsonPointerElem	   *elem = &(jsonptr->elem[i]);

		/*
		 * parse again from where this key started because this time around
		 * we need to convert the ~0 and ~1 escapes into the literal ~ and /
		 */
		cp = &input[elem->keyoff];
		elem->keyoff = result.len;
		while (true)
		{
			if (*cp == '/' || *cp == '\0')
				/* unescaped '/' or NUL so this is the end of this key */
				break;

			if (*cp == '~')
			{
				/* escape character, process according to the next */
				cp++;
				if (*cp == '0')
					appendStringInfoChar(&result, '~');
				else if (*cp == '1')
					appendStringInfoChar(&result, '/');
				else
					elog(ERROR, "invalid jsonpointer escape sequence");
				cp++;
				continue;
			}

			/* nothing special, append verbatim */
			appendStringInfoChar(&result, *cp++);
		}

		/*
		 * record the length of this key string and terminate it with NUL.
		 * (I don't know if the NUL termnination will become useful at some
		 * point, but I rather have it now than have to change the on-disk
		 * format of the jsonpointer data type later).
		 */
		elem->keylen = result.len - elem->keyoff;
		appendStringInfoChar(&result, '\0');
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
	StringInfoData	result;
	int				i;

	/* make sure we have a detoasted, plain jsonpointer object to work on */
	jsonptr = (JsonPointer *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

	/* initialize the output C-string result buffer */
	initStringInfo(&result);

	/* process all elements in the jsonpointer */
	for (i = 0; i < jsonptr->n_elem; i++)
	{
		JsonPointerElem	   *elem = &(jsonptr->elem[i]);
		char			   *cp;

		/*
		 * for each of the path elements we add a '/' and the string
		 * representation of their key, encoding '~' and '/' with the
		 * proper '~0' and '~1'.
		 */
		appendStringInfoChar(&result, '/');
		for (cp = (char *)jsonptr + elem->keyoff; *cp; cp++)
		{
			if (*cp == '~')
			{
				appendStringInfoChar(&result, '~');
				appendStringInfoChar(&result, '0');
			}
			else if (*cp == '/')
			{
				appendStringInfoChar(&result, '~');
				appendStringInfoChar(&result, '1');
			}
			else
			{
				appendStringInfoChar(&result, *cp);
			}
		}
	}

	/* nothing else to do */
	PG_RETURN_CSTRING(result.data);
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

	result = jsonptr_get_text_datum(jb, jsonptr, &isnull);
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

	result = jsonptr_get_text_datum(jb, jsonptr, &isnull);
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

	result = jsonptr_get_text_datum(jb, jsonptr, &isnull);
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

	result = jsonptr_get_text_datum(jb, jsonptr, &isnull);
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

	result = jsonptr_get_text_datum(jb, jsonptr, &isnull);
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
 * jsonpointer_collect_elem()
 *
 * 	Suppot function to finish processing one path element.
 */
static inline void
jsonpointer_collect_elem(StringInfo result, int32 keyStart)
{
	JsonPointerElem		elem;
	JsonPointer		   *jptr = (JsonPointer *)(result->data);

	/*
	 * record the start offset of the key in the element. This is
	 * for now the offset in the original input buffer. It will later
	 * be adjusted to the offset in the actual jsonpointer Datum.
	 */
	elem.keyoff		= keyStart;

	/* add this element */
	appendBinaryStringInfoNT(result, &elem, sizeof(elem));
	jptr->n_elem++;
}

/*
 * jsonptr_get_text_datum()
 *
 * 	Convert our JsonPointer into an array of text Datums and use
 * 	jsonb_get_element() to find that value. Return it as a text
 * 	Datum for the caller to deal with any conversion that might
 * 	be necessary.
 */
static Datum
jsonptr_get_text_datum(Jsonb *jb, JsonPointer *jsonptr, bool *isnull)
{
	JsonPointerElem	   *elem;
	Datum			   *path;
	int					i;

	/* Convert all the JsonPointer elements into text Datums */
	path = palloc(sizeof(Datum) * jsonptr->n_elem);
	for (i = 0; i < jsonptr->n_elem; i++)
	{
		elem = &jsonptr->elem[i];
		path[i] = (Datum)cstring_to_text(((char *)jsonptr)+elem->keyoff);
	}

	/* Let jsonb_get_element() do the actual work */
	return jsonb_get_element(jb, path, jsonptr->n_elem, isnull, true);
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
