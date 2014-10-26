-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION streaming_lag" to load this file. \quit

DROP TABLE IF EXISTS streaming_lag_data;
CREATE TABLE streaming_lag_data (tstmp TIMESTAMPTZ);

CREATE OR REPLACE VIEW streaming_lag AS
SELECT clock_timestamp() - tstmp AS lag
  FROM streaming_lag_data;
