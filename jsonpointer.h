/* -------------------------------------------------------------------------
 *
 * jsonpointer.h
 * 		PostgreSQL data type implementing RFC6901 JSON pointer
 *
 * -------------------------------------------------------------------------
 */

#ifndef JSONPOINTER_H
#define JSONPOINTER_H

typedef struct JsonPointer
{
	int32			vl_len_;	/* varlena header (do not touch!) */
	int32			n_elem;		/* Number of path elements */
	/* properly aligned text entries follow, unless n_elem is zero */
} JsonPointer;

#endif /* JSONPOINTER_H */

