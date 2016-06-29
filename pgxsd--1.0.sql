-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgxsd" to load this file. \quit

CREATE FUNCTION pgxsd.schema_validate(doc xml, schemaname text) RETURNS void AS 'pgxsd', 'pgxsd_schema_validate' LANGUAGE C IMMUTABLE STRICT;

CREATE TABLE schemata(
    schema_location text NOT NULL,
    document xml NOT NULL,

    PRIMARY KEY (schema_location)
);
