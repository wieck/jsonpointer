# jsonpointer/Makefile
  
MODULE_big = jsonpointer
OBJS = \
	jsonpointer.o

EXTENSION = jsonpointer
DATA = jsonpointer--1.0.sql
PGFILEDESC = "jsonpointer - data type implementing RFC-6901"

#ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
#else
#...
#endif
