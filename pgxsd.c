#include "postgres.h"

#include "fmgr.h"

#ifndef USE_LIBXML
#error PostgreSQL must be configured with libxml support in order to build pgxsd
#endif


PG_MODULE_MAGIC;




void _PG_init(void);
void _PG_fini(void);


/*
 * Module load callback
 */
void
_PG_init(void)
{
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
}


