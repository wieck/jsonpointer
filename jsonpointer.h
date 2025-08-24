/* -------------------------------------------------------------------------
 *
 * jsonpointer.h
 * 		PostgreSQL data type implementing RFC6901 JSON pointer
 *
 * -------------------------------------------------------------------------
 */

#ifndef JSONPOINTER_H
#define JSONPOINTER_H

typedef struct JsonPointerElem
{
	int32	keyoff;		/* offset of key in detoasted Datum */
	int32	keylen;		/* length of key */
} JsonPointerElem;

typedef struct JsonPointer
{
	int32			vl_len_;	/* varlena header (do not touch!) */
	int32			n_elem;		/* Number of path elements */
	JsonPointerElem	elem[];		
} JsonPointer;

#endif /* JSONPOINTER_H */

