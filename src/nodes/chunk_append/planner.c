/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */

#include <postgres.h>
#include <catalog/pg_namespace.h>
#include <nodes/extensible.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/appendinfo.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/placeholder.h>
#include <optimizer/planmain.h>
#include <optimizer/prep.h>
#include <optimizer/subselect.h>
#include <optimizer/tlist.h>
#include <parser/parsetree.h>

#include "guc.h"
#include "import/planner.h"
#include "nodes/chunk_append/chunk_append.h"
#include "nodes/chunk_append/transform.h"
#include "nodes/modify_hypertable.h"
#include "nodes/vector_agg.h"

static Sort *make_sort(Plan *lefttree, int numCols, AttrNumber *sortColIdx, Oid *sortOperators,
					   Oid *collations, bool *nullsFirst);
static Plan *adjust_childscan(PlannerInfo *root, Plan *plan, Path *path, List *pathkeys,
							  List *tlist, AttrNumber *sortColIdx);

static CustomScanMethods chunk_append_plan_methods = {
	.CustomName = "ChunkAppend",
	.CreateCustomScanState = ts_chunk_append_state_create,
};

bool
ts_is_chunk_append_plan(Plan *plan)
{
	if (IsA(plan, Result))
	{
		if (castNode(Result, plan)->plan.lefttree &&
			IsA(castNode(Result, plan)->plan.lefttree, CustomScan))
		{
			return castNode(CustomScan, castNode(Result, plan)->plan.lefttree)->methods ==
				   &chunk_append_plan_methods;
		}
		return false;
	}
	return IsA(plan, CustomScan) &&
		   castNode(CustomScan, plan)->methods == &chunk_append_plan_methods;
}

void
_chunk_append_init(void)
{
	TryRegisterCustomScanMethods(&chunk_append_plan_methods);
}

static Plan *
adjust_childscan(PlannerInfo *root, Plan *plan, Path *path, List *pathkeys, List *tlist,
				 AttrNumber *sortColIdx)
{
	int childSortCols;
	Oid *sortOperators;
	Oid *collations;
	bool *nullsFirst;
	AttrNumber *childColIdx;

	/* Compute sort column info, and adjust subplan's tlist as needed */
	plan = ts_prepare_sort_from_pathkeys(plan,
										 pathkeys,
										 path->parent->relids,
										 sortColIdx,
										 true,
										 &childSortCols,
										 &childColIdx,
										 &sortOperators,
										 &collations,
										 &nullsFirst);

	/* inject sort node if child sort order does not match desired order */
	if (!pathkeys_contained_in(pathkeys, path->pathkeys))
	{
		Assert(!IsA(plan, Sort));

		plan = (Plan *)
			make_sort(plan, childSortCols, childColIdx, sortOperators, collations, nullsFirst);
	}
	return plan;
}

Plan *
ts_chunk_append_plan_create(PlannerInfo *root, RelOptInfo *rel, CustomPath *path, List *tlist,
							List *clauses, List *custom_plans)
{
	ListCell *lc_child;
	List *parent_clauses = NIL;
	List *chunk_ri_clauses = NIL;
	List *chunk_rt_indexes = NIL;
	List *sort_options = NIL;
	List *custom_private = NIL;
	uint32 limit = 0;
	List *orig_tlist = NIL;

	ChunkAppendPath *capath = (ChunkAppendPath *) path;
	CustomScan *cscan = makeNode(CustomScan);

	cscan->flags = path->flags;
	cscan->methods = &chunk_append_plan_methods;
	cscan->scan.scanrelid = rel->relid;

	orig_tlist = ts_build_path_tlist(root, (Path *) path);
	tlist = orig_tlist;

	/*
	 * If this is a child of ModifyHypertable we need to adjust
	 * targetlists to not have any ROWID_VAR references as postgres
	 * asserts that scan targetlists do not have them in setrefs.c
	 *
	 * We keep orig_tlist unaltered to let adjust_appendrel_attrs()
	 * replace ROWID_VARs for chunks' targetlists (it would assert
	 * trying to modify a "wholerow" target entry that has already
	 * been adjusted by ts_replace_rowid_vars(); we see these in
	 * foreign tables).
	 */
	if (root->parse->commandType != CMD_SELECT)
		tlist = ts_replace_rowid_vars(root, tlist, rel->relid);

	cscan->scan.plan.targetlist = tlist;

	ListCell *lc_plan, *lc_path;
	forboth (lc_path, path->custom_paths, lc_plan, custom_plans)
	{
		Plan *child_plan = lfirst(lc_plan);
		Path *child_path = lfirst(lc_path);

		/* push down targetlist to children */
		if (child_path->parent->reloptkind == RELOPT_OTHER_MEMBER_REL)
		{
			/* if this is an append child we need to adjust targetlist references */
			AppendRelInfo *appinfo = ts_get_appendrelinfo(root, child_path->parent->relid, false);

			child_plan->targetlist =
				castNode(List, adjust_appendrel_attrs(root, (Node *) orig_tlist, 1, &appinfo));
		}
		else
		{
			/*
			 * This can also be a MergeAppend path building the entire
			 * hypertable, in case we have a single partial chunk.
			 */
			child_plan->targetlist = tlist;
		}
	}

	if (path->path.pathkeys != NIL)
	{
		/*
		 * If this is an ordered append node we need to ensure the columns
		 * required for sorting are present in the targetlist and all children
		 * return sorted output. Children not returning sorted output will be
		 * wrapped in a sort node.
		 */
		int numCols;
		AttrNumber *sortColIdx;
		Oid *sortOperators;
		Oid *collations;
		bool *nullsFirst;
		List *pathkeys = path->path.pathkeys;
		List *sort_indexes = NIL;
		List *sort_ops = NIL;
		List *sort_collations = NIL;
		List *sort_nulls = NIL;
		int i;

		/* Compute sort column info, and adjust MergeAppend's tlist as needed */
		ts_prepare_sort_from_pathkeys(&cscan->scan.plan,
									  pathkeys,
									  path->path.parent->relids,
									  NULL,
									  true,
									  &numCols,
									  &sortColIdx,
									  &sortOperators,
									  &collations,
									  &nullsFirst);

		/*
		 * collect sort information to make available to explain
		 */
		for (i = 0; i < numCols; i++)
		{
			sort_indexes = lappend_oid(sort_indexes, sortColIdx[i]);
			sort_ops = lappend_oid(sort_ops, sortOperators[i]);
			sort_collations = lappend_oid(sort_collations, collations[i]);
			sort_nulls = lappend_oid(sort_nulls, nullsFirst[i]);
		}

		sort_options = list_make4(sort_indexes, sort_ops, sort_collations, sort_nulls);

		forboth (lc_path, path->custom_paths, lc_plan, custom_plans)
		{
			/*
			 * If the planner injected a Result node to do projection
			 * we can safely remove the Result node if it does not have
			 * a one-time filter because ChunkAppend can do projection.
			 */
			if (IsA(lfirst(lc_plan), Result) &&
				castNode(Result, lfirst(lc_plan))->resconstantqual == NULL)
				lfirst(lc_plan) = ((Plan *) lfirst(lc_plan))->lefttree;

			/*
			 * This could be a MergeAppend due to space partitioning, or
			 * due to partially compressed chunks. The MergeAppend plan adds
			 * sort to it children, and has the proper sorting itself, so no
			 * need to do anything for it.
			 * We can also have plain chunk scans here which might require a
			 * Sort.
			 */
			if (!IsA(lfirst(lc_plan), MergeAppend))
			{
				lfirst(lc_plan) = adjust_childscan(root,
												   lfirst(lc_plan),
												   lfirst(lc_path),
												   path->path.pathkeys,
												   orig_tlist,
												   sortColIdx);
			}
		}
	}

	/* decouple input tlist from output tlist in case output tlist gets modified later */
	cscan->custom_scan_tlist = list_copy(tlist);
	cscan->custom_plans = custom_plans;

	/*
	 * If we do either startup or runtime exclusion, we need to pass restrictinfo
	 * clauses into executor.
	 */
	if (capath->startup_exclusion || capath->runtime_exclusion_children)
	{
		foreach (lc_child, cscan->custom_plans)
		{
			Scan *scan = ts_chunk_append_get_scan_plan(lfirst(lc_child));

			if (scan == NULL || scan->scanrelid == 0)
			{
				chunk_ri_clauses = lappend(chunk_ri_clauses, NIL);
				chunk_rt_indexes = lappend_oid(chunk_rt_indexes, 0);
			}
			else
			{
				List *chunk_clauses = NIL;
				ListCell *lc;
				AppendRelInfo *appinfo = ts_get_appendrelinfo(root, scan->scanrelid, false);

				foreach (lc, clauses)
				{
					Node *clause = (Node *) ts_transform_cross_datatype_comparison(
						castNode(RestrictInfo, lfirst(lc))->clause);
					clause = adjust_appendrel_attrs(root, clause, 1, &appinfo);
					chunk_clauses = lappend(chunk_clauses, clause);
				}
				chunk_ri_clauses = lappend(chunk_ri_clauses, chunk_clauses);
				chunk_rt_indexes = lappend_oid(chunk_rt_indexes, scan->scanrelid);
			}
		}

		Assert(list_length(cscan->custom_plans) == list_length(chunk_ri_clauses));
		Assert(list_length(chunk_ri_clauses) == list_length(chunk_rt_indexes));
	}

	/* pass down the parent clauses if doing parent exclusion */
	if (capath->runtime_exclusion_parent)
	{
		ListCell *lc;
		foreach (lc, clauses)
		{
			parent_clauses = lappend(parent_clauses, castNode(RestrictInfo, lfirst(lc))->clause);
		}
	}

	if (capath->pushdown_limit && capath->limit_tuples > 0)
		limit = capath->limit_tuples;

	custom_private = list_make1(list_make5_int(capath->startup_exclusion,
											   capath->runtime_exclusion_parent,
											   capath->runtime_exclusion_children,
											   limit,
											   capath->first_partial_path));
	custom_private = lappend(custom_private, chunk_ri_clauses);
	custom_private = lappend(custom_private, chunk_rt_indexes);
	custom_private = lappend(custom_private, sort_options);
	custom_private = lappend(custom_private, parent_clauses);

	cscan->custom_private = custom_private;

	return &cscan->scan.plan;
}

/*
 * make_sort --- basic routine to build a Sort plan node
 *
 * Caller must have built the sortColIdx, sortOperators, collations, and
 * nullsFirst arrays already.
 */
static Sort *
make_sort(Plan *lefttree, int numCols, AttrNumber *sortColIdx, Oid *sortOperators, Oid *collations,
		  bool *nullsFirst)
{
	Sort *node = makeNode(Sort);
	Plan *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->numCols = numCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;
	node->collations = collations;
	node->nullsFirst = nullsFirst;

	return node;
}

Scan *
ts_chunk_append_get_scan_plan(Plan *plan)
{
	if (plan == NULL)
		return NULL;

	switch (nodeTag(plan))
	{
		case T_BitmapHeapScan:
		case T_BitmapIndexScan:
		case T_CteScan:
		case T_ForeignScan:
		case T_FunctionScan:
		case T_IndexOnlyScan:
		case T_IndexScan:
		case T_SampleScan:
		case T_SeqScan:
		case T_SubqueryScan:
		case T_TidScan:
		case T_ValuesScan:
		case T_WorkTableScan:
		case T_TidRangeScan:
			return (Scan *) plan;
		case T_CustomScan:
		{
			CustomScan *custom = castNode(CustomScan, plan);
			if (custom->scan.scanrelid > 0)
			{
				/*
				 * The custom plan node is a scan itself. This handles the
				 * DecompressChunk node.
				 */
				return (Scan *) plan;
			}

			if (strcmp(custom->methods->CustomName, VECTOR_AGG_NODE_NAME) == 0)
			{
				/*
				 * This is a vectorized aggregation node, we have to recurse
				 * into its child, similar to the normal aggregation node.
				 *
				 * Unfortunately we have to hardcode the node name here, because
				 * we can't depend on the TSL library.
				 */
				return ts_chunk_append_get_scan_plan(linitial(custom->custom_plans));
			}
			break;
		}
		case T_Sort:
		case T_Result:
		case T_Agg:
			if (plan->lefttree != NULL)
			{
				Assert(plan->righttree == NULL);
				/* Let ts_chunk_append_get_scan_plan handle the subplan */
				return ts_chunk_append_get_scan_plan(plan->lefttree);
			}
			break;
		default:
			break;
	}

	return NULL;
}
