-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

\c :TEST_DBNAME :ROLE_SUPERUSER
CREATE FUNCTION ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish(INT, INT) RETURNS VOID
AS :MODULE_PATHNAME LANGUAGE C VOLATILE;

CREATE FUNCTION ts_bgw_params_create() RETURNS VOID
AS :MODULE_PATHNAME LANGUAGE C VOLATILE;

CREATE FUNCTION ts_bgw_params_destroy() RETURNS VOID
AS :MODULE_PATHNAME LANGUAGE C VOLATILE;

CREATE FUNCTION ts_bgw_params_reset_time(set_time BIGINT, wait BOOLEAN) RETURNS VOID
AS :MODULE_PATHNAME LANGUAGE C VOLATILE;

ALTER DATABASE :TEST_DBNAME OWNER TO :ROLE_DEFAULT_PERM_USER;
GRANT EXECUTE ON FUNCTION pg_reload_conf TO :ROLE_DEFAULT_PERM_USER;
GRANT ALTER SYSTEM, SET ON PARAMETER timescaledb.bgw_log_level TO :ROLE_DEFAULT_PERM_USER;

-- These are needed to set up the test scheduler
CREATE TABLE public.bgw_dsm_handle_store(handle BIGINT);
INSERT INTO public.bgw_dsm_handle_store VALUES (0);
SELECT ts_bgw_params_create();

-- Test scheduler automatically writes to this table by name, so
-- create it.
CREATE TABLE public.bgw_log(
    msg_no INT,
    mock_time BIGINT,
    application_name TEXT,
    msg TEXT
);

CREATE VIEW cleaned_bgw_log AS
    SELECT msg_no, application_name,
    	   regexp_replace(regexp_replace(msg, '(Wait until|started at|execution time|database) [0-9]+(\.[0-9]+)?', '\1 (RANDOM)', 'g'), 'background worker "[^"]+"','connection') AS msg
      FROM bgw_log ORDER BY mock_time, application_name COLLATE "C", msg_no;

-- Remove all default jobs
DELETE FROM _timescaledb_config.bgw_job WHERE TRUE;
TRUNCATE _timescaledb_internal.bgw_job_stat;

--
-- Set bgw log level and reload config.
--
-- Debug messages should be in log now which it wasn't before.
--
-- We change user to make sure that granting SET and ALTER SYSTEM
-- privileges to the default user actually works.
SET ROLE :ROLE_DEFAULT_PERM_USER;
ALTER DATABASE :TEST_DBNAME SET timescaledb.bgw_log_level = 'DEBUG1';
SELECT pg_reload_conf();

RESET ROLE;
SELECT ts_bgw_params_reset_time(0, false);
INSERT INTO _timescaledb_config.bgw_job(
       application_name,
       schedule_interval,
       max_runtime,
       max_retries,
       retry_period,
       proc_schema,
       proc_name,
       owner,
       scheduled,
       fixed_schedule
) VALUES (
	'test_job_1b',		--application_name
	INTERVAL '100ms',	--schedule_interval
	INTERVAL '100s',	--max_runtime
	5,			--max_retries
	INTERVAL '1s',		--retry_period
	'public',		--proc_schema
	'bgw_test_job_1',	--proc_name
	CURRENT_ROLE::regrole,	--owner
	TRUE,			--scheduled
	FALSE			--fixed_schedule
) RETURNING id AS job_id \gset

SELECT ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish(25, 0);
SELECT * FROM cleaned_bgw_log;

-- We test that we can set it to FATAL, which removed LOG level
-- entries from the log.
ALTER DATABASE :TEST_DBNAME SET timescaledb.bgw_log_level = 'FATAL';
SELECT pg_reload_conf();

\c :TEST_DBNAME :ROLE_SUPERUSER
TRUNCATE bgw_log;
SELECT ts_bgw_params_reset_time(0, false);
SELECT ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish(25, 0);
SELECT * FROM cleaned_bgw_log;

-- We test that we can set it to ERROR.
ALTER DATABASE :TEST_DBNAME SET timescaledb.bgw_log_level = 'ERROR';
SELECT pg_reload_conf();

\c :TEST_DBNAME :ROLE_SUPERUSER
TRUNCATE bgw_log;
SELECT ts_bgw_params_reset_time(0, false);
SELECT ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish(25, 0);
SELECT * FROM cleaned_bgw_log;

-- Reset the log level and check that normal entries are showing up
-- again.
ALTER DATABASE :TEST_DBNAME RESET timescaledb.bgw_log_level;
SELECT pg_reload_conf();

\c :TEST_DBNAME :ROLE_SUPERUSER
TRUNCATE bgw_log;
SELECT ts_bgw_params_reset_time(0, false);
SELECT ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish(25, 0);
SELECT * FROM cleaned_bgw_log;

SELECT delete_job(:job_id);

SET ROLE :ROLE_DEFAULT_PERM_USER;

-- Make sure we can set the variable using ALTER SYSTEM using the
-- previous grants. We don't bother about checking that it has an
-- effect here since we already knows it works from the above code.
ALTER SYSTEM SET timescaledb.bgw_log_level TO 'DEBUG2';
ALTER SYSTEM RESET timescaledb.bgw_log_level;
