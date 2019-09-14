PLJS_VERSION = 0.0.1

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

PG_VERSION_NUM := $(shell cat `$(PG_CONFIG) --includedir`/pg_config*.h \
		   | perl -ne 'print $$1 and exit if /PG_VERSION_NUM\s+(\d+)/')

CP = cp

SRCS = pljs.c pljs_cache.c
OBJS = pljs.o pljs_cache.o deps/quickjs/libquickjs.a
MODULE_big = pljs
EXTENSION = pljs
DATA = pljs.control pljs--$(PLJS_VERSION).sql

deps/quickjs/libquickjs.a:
	cd deps/quickjs && make

#pljs.o: pljs.c pljs.h
#pljs_cache.o: pljs_cache.c pljs.h

%--$(PLJS_VERSION).sql: pljs.sql
	$(CP) pljs.sql pljs--$(PLJS_VERSION).sql


include $(PGXS)
