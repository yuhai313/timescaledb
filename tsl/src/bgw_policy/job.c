/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <postgres.h>
#include <funcapi.h>
#include <utils/timestamp.h>
#include <access/xact.h>
#include <catalog/namespace.h>
#include <catalog/pg_type.h>
#include <utils/lsyscache.h>
#include <hypertable_cache.h>
#include <utils/snapmgr.h>

#include "bgw/timer.h"
#include "bgw/job_stat.h"
#include "bgw_policy/chunk_stats.h"
#include "bgw_policy/drop_chunks.h"
#include "bgw_policy/reorder.h"
#include "continuous_aggs/materialize.h"
#include "continuous_aggs/job.h"

#include "errors.h"
#include "job.h"
#include "../hypertable.h"
#include "chunk.h"
#include "dimension.h"
#include "dimension_slice.h"
#include "dimension_vector.h"
#include "errors.h"
#include "job.h"
#include "license.h"
#include "reorder.h"
#include "utils.h"
#include "drop_chunks_api.h"

#define ALTER_JOB_SCHEDULE_NUM_COLS 5
#define REORDER_SKIP_RECENT_DIM_SLICES_N 3

static void
enable_fast_restart(BgwJob *job, const char *job_name)
{
	BgwJobStat *job_stat = ts_bgw_job_stat_find(job->fd.id);

	ts_bgw_job_stat_set_next_start(job, job_stat->fd.last_start);
	elog(LOG, "the %s job is scheduled to run again immediately", job_name);
}

/*
 * Returns the ID of a chunk to reorder. Eligible chunks must be at least the
 * 3rd newest chunk in the hypertable (not entirely exact because we use the number
 * of dimension slices as a proxy for the number of chunks) and hasn't been
 * reordered recently. For this version of automatic reordering, "not reordered
 * recently" means the chunk has not been reordered at all. This information
 * is available in the bgw_policy_chunk_stats metadata table.
 */
static int
get_chunk_id_to_reorder(int32 job_id, Hypertable *ht)
{
	Dimension *time_dimension = hyperspace_get_open_dimension(ht->space, 0);
	DimensionSlice *nth_dimension =
		ts_dimension_slice_nth_latest_slice(time_dimension->fd.id,
											REORDER_SKIP_RECENT_DIM_SLICES_N);

	if (!nth_dimension)
		return -1;

	Assert(time_dimension != NULL);

	return ts_dimension_slice_oldest_chunk_without_executed_job(job_id,
																time_dimension->fd.id,
																BTLessEqualStrategyNumber,
																nth_dimension->fd.range_start,
																InvalidStrategy,
																-1);
}

bool
execute_reorder_policy(BgwJob *job, reorder_func reorder, bool fast_continue)
{
	int chunk_id;
	bool started = false;
	BgwPolicyReorder *args;
	Hypertable *ht;
	Chunk *chunk;
	int32 job_id = job->fd.id;

	if (!IsTransactionOrTransactionBlock())
	{
		started = true;
		StartTransactionCommand();
	}

	/* Get the arguments from the reorder_policy table */
	args = ts_bgw_policy_reorder_find_by_job(job_id);

	if (args == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_TS_INTERNAL_ERROR),
				 errmsg("could not run reorder policy #%d because no args in policy table",
						job_id)));

	ht = ts_hypertable_get_by_id(args->fd.hypertable_id);

	/* Find a chunk to reorder in the selected hypertable */
	chunk_id = get_chunk_id_to_reorder(args->fd.job_id, ht);

	if (chunk_id == -1)
	{
		elog(NOTICE,
			 "no chunks need reordering for hypertable %s.%s",
			 ht->fd.schema_name.data,
			 ht->fd.table_name.data);
		goto commit;
	}

	/*
	 * NOTE: We pass the Oid of the hypertable's index, and the true reorder
	 * function should translate this to the Oid of the index on the specific
	 * chunk.
	 */
	chunk = ts_chunk_get_by_id(chunk_id, 0, false);
	elog(LOG, "reordering chunk %s.%s", chunk->fd.schema_name.data, chunk->fd.table_name.data);
	reorder(chunk->table_id,
			get_relname_relid(NameStr(args->fd.hypertable_index_name),
							  get_namespace_oid(NameStr(ht->fd.schema_name), false)),
			false,
			InvalidOid,
			InvalidOid,
			InvalidOid);
	elog(LOG,
		 "completed reordering chunk %s.%s",
		 chunk->fd.schema_name.data,
		 chunk->fd.table_name.data);

	/* Now update chunk_stats table */
	ts_bgw_policy_chunk_stats_record_job_run(args->fd.job_id,
											 chunk_id,
											 ts_timer_get_current_timestamp());

	if (fast_continue && get_chunk_id_to_reorder(args->fd.job_id, ht) != -1)
		enable_fast_restart(job, "reorder");

commit:
	if (started)
		CommitTransactionCommand();

	return true;
}

bool
execute_drop_chunks_policy(int32 job_id)
{
	bool started = false;
	BgwPolicyDropChunks *args;
	Oid table_relid;
	Hypertable *hypertable;
	Cache *hcache;
	Dimension *open_dim;

	if (!IsTransactionOrTransactionBlock())
	{
		started = true;
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
	}

	/* Get the arguments from the drop_chunks_policy table */
	args = ts_bgw_policy_drop_chunks_find_by_job(job_id);

	if (args == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_TS_INTERNAL_ERROR),
				 errmsg("could not run drop_chunks policy #%d because no args in policy table",
						job_id)));

	table_relid = ts_hypertable_id_to_relid(args->fd.hypertable_id);
	hcache = ts_hypertable_cache_pin();
	hypertable = ts_hypertable_cache_get_entry(hcache, table_relid);
	/* First verify that the hypertable corresponds to a valid table */
	if (hypertable == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_TS_HYPERTABLE_NOT_EXIST),
				 errmsg("could not run drop_chunks policy #%d because \"%s\" is not a hypertable",
						job_id,
						get_rel_name(table_relid))));

	open_dim = hyperspace_get_open_dimension(hypertable->space, 0);
	ts_chunk_do_drop_chunks(table_relid,
							ts_interval_subtract_from_now(&args->fd.older_than, open_dim),
							(Datum) 0,
							ts_dimension_get_partition_type(open_dim),
							InvalidOid,
							args->fd.cascade,
							args->fd.cascade_to_materializations,
							LOG);

	ts_cache_release(hcache);
	elog(LOG, "completed dropping chunks");

	if (started)
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	return true;
}

static bool
execute_materialize_continuous_aggregate(BgwJob *job)
{
	bool started = false;
	int32 materialization_id;
	bool finshed_all_materialization;

	if (!IsTransactionOrTransactionBlock())
	{
		started = true;
		StartTransactionCommand();
	}

	materialization_id = ts_continuous_agg_job_find_materializtion_by_job_id(job->fd.id);
	if (materialization_id < 0)
		elog(ERROR, "cannot find continuous aggregate for job %d", job->fd.id);

	CommitTransactionCommand();

	/* always materialize verbosely for now */
	finshed_all_materialization = continuous_agg_materialize(materialization_id, true);

	StartTransactionCommand();

	if (!finshed_all_materialization)
		enable_fast_restart(job, "materialize continuous aggregate");

	if (started)
		CommitTransactionCommand();

	return true;
}

static bool
bgw_policy_job_requires_enterprise_license(BgwJob *job)
{
	license_print_expiration_warning_if_needed();

	switch (job->bgw_type)
	{
		case JOB_TYPE_REORDER:
			return true;
		case JOB_TYPE_DROP_CHUNKS:
			return true;
		case JOB_TYPE_CONTINUOUS_AGGREGATE:
			return false;
		default:
			elog(ERROR,
				 "scheduler could not determine the license type for job type: \"%s\"",
				 NameStr(job->fd.job_type));
	}
	pg_unreachable();
}

bool
tsl_bgw_policy_job_execute(BgwJob *job)
{
	if (bgw_policy_job_requires_enterprise_license(job))
		license_enforce_enterprise_enabled();
	license_print_expiration_warning_if_needed();

	switch (job->bgw_type)
	{
		case JOB_TYPE_REORDER:
			return execute_reorder_policy(job, reorder_chunk, true);
		case JOB_TYPE_DROP_CHUNKS:
			return execute_drop_chunks_policy(job->fd.id);
		case JOB_TYPE_CONTINUOUS_AGGREGATE:
			return execute_materialize_continuous_aggregate(job);
		default:
			elog(ERROR,
				 "scheduler tried to run an invalid job type: \"%s\"",
				 NameStr(job->fd.job_type));
	}
	pg_unreachable();
}

Datum
bgw_policy_alter_job_schedule(PG_FUNCTION_ARGS)
{
	BgwJob *job;
	TupleDesc tupdesc;
	Datum values[ALTER_JOB_SCHEDULE_NUM_COLS];
	bool nulls[ALTER_JOB_SCHEDULE_NUM_COLS] = { false };
	HeapTuple tuple;

	int job_id = PG_GETARG_INT32(0);
	bool if_exists = PG_GETARG_BOOL(5);

	license_enforce_enterprise_enabled();
	license_print_expiration_warning_if_needed();

	/* First get the job */
	job = ts_bgw_job_find(job_id, CurrentMemoryContext, false);

	if (!job)
	{
		if (if_exists)
		{
			ereport(NOTICE,
					(errmsg("cannot alter policy schedule, policy #%d not found, skipping",
							job_id)));
			PG_RETURN_NULL();
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("cannot alter policy schedule, policy #%d not found", job_id)));
	}

	ts_bgw_job_permission_check(job);

	if (!PG_ARGISNULL(1))
		job->fd.schedule_interval = *PG_GETARG_INTERVAL_P(1);
	if (!PG_ARGISNULL(2))
		job->fd.max_runtime = *PG_GETARG_INTERVAL_P(2);
	if (!PG_ARGISNULL(3))
		job->fd.max_retries = PG_GETARG_INT32(3);
	if (!PG_ARGISNULL(4))
		job->fd.retry_period = *PG_GETARG_INTERVAL_P(4);

	ts_bgw_job_update_by_id(job_id, job);

	/* Now look up the job and return it */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);
	values[0] = Int32GetDatum(job->fd.id);
	values[1] = IntervalPGetDatum(&job->fd.schedule_interval);
	values[2] = IntervalPGetDatum(&job->fd.max_runtime);
	values[3] = Int32GetDatum(job->fd.max_retries);
	values[4] = IntervalPGetDatum(&job->fd.retry_period);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	return HeapTupleGetDatum(tuple);
}
