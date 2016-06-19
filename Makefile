MODULE_big = pgxsd
OBJS = pgxsd.o

EXTENSION = pgxsd
DATA = pgxsd--1.0.sql

ifdef NO_PGXS
subdir = contrib/pgxsd
top_builddir = ../..
include $(top_builddir)/src/Makefile.Global
include $(top_srcdir)/contrib/contrib-global.mk
else
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
endif
