#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/xml.h"

#ifdef USE_LIBXML
#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlerror.h>
#include <libxml/parserInternals.h>

#define LIBXML_SCHEMAS_ENABLED
#include <libxml/xmlschemastypes.h>

#else
#error PostgreSQL must be configured with libxml support in order to build pgxsd
#endif


PG_MODULE_MAGIC;


static void
pgxsd_schema_validity_error(void *ctx, const char *msg, ...)
{
	elog(ERROR, "NOT VALID: %s", msg);
}

static SPIPlanPtr load_xsd_plan = NULL;
static const char *load_xsd_sql = "SELECT schemata.document FROM pgxsd.schemata WHERE schemata.schema_location = $1";

static xmlParserInputPtr
pgxsd_external_entity_loader(const char *URL, const char *ID, xmlParserCtxtPtr ctxt)
{
	int ret;
	Datum values[1];

	SPITupleTable *tuptable;
	TupleDesc tupdesc;
	HeapTuple htup;

	char *docstr;
	xmlParserInputBufferPtr docbuf;

	(void) ID; /* XXX ??? */

	if (load_xsd_plan == NULL)
	{
		Oid argtypes[] = {TEXTOID};
		load_xsd_plan = SPI_prepare(load_xsd_sql, 1, argtypes);
		if (load_xsd_plan == NULL)
		{
			/* internal error */
			elog(ERROR, "SPI_prepare returned %d", SPI_result);
		}

		/* hold onto the plan */
		if ((ret = SPI_keepplan(load_xsd_plan)) != 0)
		{
			/* internal error */
			elog(ERROR, "SPI_keepplan failed with error code %d", ret);
		}
	}

	values[0] = CStringGetTextDatum(URL);
	ret = SPI_execute_plan(load_xsd_plan, values, NULL, true, 0);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "internal error: SPI_execute_plan returned %d", ret);
	if (SPI_processed == 0)
	{
		/* TODO: improve this error */
		elog(ERROR, "XML schema \"%s\" could not be located", URL);
	}
	else if (SPI_processed != 1)
	{
		/* internal error */
		elog(ERROR, "unexpected SPI_processed value %d", (int) SPI_processed);
	}
	if (SPI_tuptable == NULL)
	{
		/* internal error */
		elog(ERROR, "SPI_tuptable is NULL");
	}
	tuptable = SPI_tuptable;
	tupdesc = tuptable->tupdesc;
	if (tupdesc->natts != 1)
	{
		/* internal error */
		elog(ERROR, "unexpected SPI TupleDesc natts %d", tupdesc->natts);
	}
	htup = tuptable->vals[0];

	/*
	 * XXX FIXME all this crap is probably leaked right now.  The libxml2 docs
	 * aren't very clear on how exactly this is supposed to work. :-(
	 */

	/* XXX FIXME this probably doesn't work for anything other than UTF-8 */
	docstr = SPI_getvalue(htup, tupdesc, 1);
	docbuf = xmlParserInputBufferCreateMem(docstr, strlen(docstr), XML_CHAR_ENCODING_UTF8);
	return xmlNewIOInputStream(ctxt, docbuf, XML_CHAR_ENCODING_UTF8);
}


Datum pgxsd_schema_validate(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pgxsd_schema_validate);

Datum
pgxsd_schema_validate(PG_FUNCTION_ARGS)
{
	xmlChar    *utf8string;
	int ret;
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlExternalEntityLoader oldxmlel = NULL;
	volatile xmlSchemaParserCtxtPtr sctxt = NULL;
	volatile xmlSchemaPtr schema = NULL;
	volatile xmlParserCtxtPtr pctxt = NULL;
	volatile xmlDocPtr doc = NULL;
	volatile xmlSchemaValidCtxtPtr svctxt = NULL;

	utf8string = (xmlChar *) text_to_cstring(PG_GETARG_TEXT_PP(0));

	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_WELLFORMED);
	/*
	 * Override PG's external entity loader with ours.  While we don't strictly
	 * need it in order to read the input schema (since we could just use
	 * xmlCtxtReadDoc), we really have to have one in case the schemas have any
	 * imports.
	 */
	oldxmlel = xmlGetExternalEntityLoader();
	xmlSetExternalEntityLoader(pgxsd_external_entity_loader);

	PG_TRY();
	{
		char *schemaname;

		if ((ret = SPI_connect()) < 0)
		{
			/* internal error */
			elog(ERROR, "SPI_connect returned %d", ret);
		}

		xmlInitParser();

		schemaname = text_to_cstring(PG_GETARG_TEXT_PP(1));
		sctxt = xmlSchemaNewParserCtxt(schemaname);
		if (sctxt == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate schema parser context");

		schema = xmlSchemaParse(sctxt);
		if (schema == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"invalid XML schema");

		pctxt = xmlNewParserCtxt();
		if (pctxt == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate parser context");

		doc = xmlCtxtReadDoc(pctxt, utf8string,
							 NULL,
							 "UTF-8",
							 XML_PARSE_NOENT | XML_PARSE_DTDATTR);
		if (doc == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"invalid XML document");

		svctxt = xmlSchemaNewValidCtxt(schema);
		if (svctxt == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate schema validation context");

		/* TODO: warnings */
		xmlSchemaSetValidErrors(svctxt,
								pgxsd_schema_validity_error,
								pgxsd_schema_validity_error,
								NULL);

		ret = xmlSchemaValidateDoc(svctxt, doc);
		if (ret == -1 || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"uh oh spaghetti O's");
		elog(INFO, "ret: %d", ret);
	}
	PG_CATCH();
	{
		if (doc != NULL)
			xmlFreeDoc(doc);
		if (pctxt != NULL)
			xmlFreeParserCtxt(pctxt);
		if (schema != NULL)
			xmlSchemaFree(schema);
		if (sctxt != NULL)
			xmlSchemaFreeParserCtxt(sctxt);
		if (svctxt != NULL)
			xmlSchemaFreeValidCtxt(svctxt);

		xmlSetExternalEntityLoader(oldxmlel);

		pg_xml_done(xmlerrcxt, true);

		SPI_finish();

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlFreeDoc(doc);
	xmlFreeParserCtxt(pctxt);
	xmlSchemaFree(schema);
	xmlSchemaFreeParserCtxt(sctxt);
	xmlSchemaFreeValidCtxt(svctxt);

	xmlSetExternalEntityLoader(oldxmlel);

	pg_xml_done(xmlerrcxt, false);

	SPI_finish();

	PG_RETURN_VOID();
}
