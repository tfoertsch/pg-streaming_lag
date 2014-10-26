MODULES = streaming_lag
OBJS = streaming_lag.o

EXTENSION = streaming_lag
EXVERSION = $(shell sed -n \
              "/^default_version[[:space:]]*=/s/.*'\(.*\)'.*/\1/p" \
              streaming_lag.control)

DATA = streaming_lag--$(EXVERSION).sql

PG_CONFIG = pg_config

# verify version is 9.3 or later

PG93 = $(shell $(PG_CONFIG) --version | \
         (IFS="$${IFS}." read x v m s; expr $$v \* 100 + $$m \>= 903))

ifneq ($(PG93),1)
$(error Requires PostgreSQL 9.3 or later)
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)


