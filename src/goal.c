#include <assert.h>
#include <stdio.h>

// libsolv
#include "solv/solver.h"
#include "solv/solverdebug.h"
#include "solv/util.h"

// hawkey
#include "goal.h"
#include "iutil.h"
#include "query_internal.h"
#include "package_internal.h"
#include "sack_internal.h"

struct _HyGoal {
    HySack sack;
    Queue job;
    Solver *solv;
    Transaction *trans;
};

static int
solve(HyGoal goal, int flags)
{
    HySack sack = goal->sack;
    Pool *pool = sack_pool(sack);
    Solver *solv = solver_create(pool);

    goal->solv = solv;
    sack_make_provides_ready(sack);
    if (flags & HY_ALLOW_UNINSTALL)
	solver_set_flag(solv, SOLVER_FLAG_ALLOW_UNINSTALL, 1);

    /* turn off implicit obsoletes for installonly packages */
    for (int i = 0; i < sack->installonly.count; i++)
	queue_push2(&goal->job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_PROVIDES,
		    sack->installonly.elements[i]);

    /* installonly notwithstanding, process explicit obsoletes */
    solver_set_flag(solv, SOLVER_FLAG_KEEP_EXPLICIT_OBSOLETES, 1);

    if (solver_solve(solv, &goal->job))
	return 1;

    goal->trans = solver_create_transaction(solv);
    return 0;
}

static HyPackageList
list_results(HyGoal goal, Id type_filter)
{
    Pool *pool = sack_pool(goal->sack);
    Queue transpkgs;
    Transaction *trans = goal->trans;
    HyPackageList plist;

    assert(trans);
    queue_init(&transpkgs);
    plist = hy_packagelist_create();

    for (int i = 0; i < trans->steps.count; ++i) {
	Id p = trans->steps.elements[i];
	Id type = transaction_type(trans, p, SOLVER_TRANSACTION_SHOW_ACTIVE);

	if (type == type_filter)
	    hy_packagelist_push(plist, package_create(pool, p));
    }
    return plist;
}

HyGoal
hy_goal_create(HySack sack)
{
    HyGoal goal = solv_calloc(1, sizeof(*goal));
    goal->sack = sack;
    queue_init(&goal->job);
    return goal;
}

void
hy_goal_free(HyGoal goal)
{
    if (goal->trans)
	transaction_free(goal->trans);
    if (goal->solv)
	solver_free(goal->solv);
    queue_free(&goal->job);
    solv_free(goal);
}

int
hy_goal_downgrade_to(HyGoal goal, HyPackage new_pkg)
{
    return hy_goal_install(goal, new_pkg);
}

int
hy_goal_erase(HyGoal goal, HyPackage pkg)
{
#ifndef NDEBUG
    Pool *pool = sack_pool(goal->sack);
    assert(pool->installed &&
	   pool_id2solvable(pool, package_id(pkg))->repo == pool->installed);
#endif
    queue_push2(&goal->job, SOLVER_SOLVABLE|SOLVER_ERASE, package_id(pkg));
    return 0;
}

int
hy_goal_erase_query(HyGoal goal, HyQuery query)
{
    return query2job(query, &goal->job, SOLVER_ERASE);
}

int
hy_goal_install(HyGoal goal, HyPackage new_pkg)
{
    queue_push2(&goal->job, SOLVER_SOLVABLE|SOLVER_INSTALL, package_id(new_pkg));
    return 0;
}

int
hy_goal_install_query(HyGoal goal, HyQuery  query)
{
    return query2job(query, &goal->job, SOLVER_INSTALL);
}

int
hy_goal_upgrade_all(HyGoal goal)
{
    queue_push2(&goal->job, SOLVER_UPDATE|SOLVER_SOLVABLE_ALL, 0);
    return 0;
}

int
hy_goal_upgrade_to(HyGoal goal, HyPackage new_pkg)
{
    return hy_goal_upgrade_to_flags(goal, new_pkg, 0);
}

int
hy_goal_upgrade_query(HyGoal goal, HyQuery query)
{
    return query2job(query, &goal->job, SOLVER_UPDATE);
}

int
hy_goal_upgrade_to_flags(HyGoal goal, HyPackage new_pkg, int flags)
{
    int count = 0;

    if (flags & HY_CHECK_INSTALLED) {
	HyQuery q = hy_query_create(goal->sack);
	const char *name = hy_package_get_name(new_pkg);
	HyPackageList installed;

	hy_query_filter(q, HY_PKG_NAME, HY_EQ, name);
	hy_query_filter(q, HY_PKG_REPO, HY_EQ, HY_SYSTEM_REPO_NAME);
	installed = hy_query_run(q);
	count = hy_packagelist_count(installed);
	hy_packagelist_free(installed);
	hy_query_free(q);
	if (!count)
	    return 1;
    }

    return hy_goal_install(goal, new_pkg);
}

int
hy_goal_go(HyGoal goal)
{
    return hy_goal_go_flags(goal, 0);
}

int
hy_goal_go_flags(HyGoal goal, int flags)
{
    assert(goal->solv == NULL); /* only allow goal_go() once */
    if (solve(goal, flags))
	return 1;

#if 0
    Transaction *trans = goal->trans;
    Pool *pool = sack_pool(goal->sack);

    assert(trans);
    transaction_print(trans);
    for (int i = 0; i < trans->steps.count; ++i) {
	Id p = trans->steps.elements[i];
	Solvable *s = pool_id2solvable(pool, p);
	Id type = transaction_type(trans, p, SOLVER_TRANSACTION_RPM_ONLY);
	switch (type) {
	case SOLVER_TRANSACTION_ERASE:
	    printf("erasing id %d\n", p);
	    break;
	case SOLVER_TRANSACTION_INSTALL:
	    printf("installing %d\n", p);
	    printf("\t %s %s\n", s->repo->name,
		   solvable_get_location(s, NULL));
	    break;
	default:
	    printf("unrecognized type 0x%x for %d\n", type, p);
	    printf("\t %s\n", pool_solvable2str(pool, s));
	    break;
	}
    }
#endif
    return 0;
}

int
hy_goal_count_problems(HyGoal goal)
{
    assert(goal->solv);
    return solver_problem_count(goal->solv);
}

/**
 * String describing the encountered solving problem 'i'.
 *
 * Caller is responsible for freeing the returned string using hy_free().
 */
char *
hy_goal_describe_problem(HyGoal goal, unsigned i)
{
    const int count = hy_goal_count_problems(goal);
    Id rid, source, target, dep;
    SolverRuleinfo type;

    assert(i < count);
    // this libsolv interface indexes from 1 (we do from 0), so:
    rid = solver_findproblemrule(goal->solv, i + 1);
    type = solver_ruleinfo(goal->solv, rid, &source, &target, &dep);

    const char *problem = solver_problemruleinfo2str(goal->solv,
						     type, source, target, dep);
    return solv_strdup(problem);
}

/**
 * Write all the solving decisions to the hawkey logfile.
 */
int
hy_goal_log_decisions(HyGoal goal)
{
    if (goal->solv == NULL)
	return 1;
    solver_printdecisionq(goal->solv, SOLV_DEBUG_RESULT);
    return 0;
}

HyPackageList
hy_goal_list_erasures(HyGoal goal)
{
    return list_results(goal, SOLVER_TRANSACTION_ERASE);
}

HyPackageList
hy_goal_list_installs(HyGoal goal)
{
    return list_results(goal, SOLVER_TRANSACTION_INSTALL);
}

HyPackageList
hy_goal_list_upgrades(HyGoal goal)
{
    return list_results(goal, SOLVER_TRANSACTION_UPGRADE);
}

HyPackageList
hy_goal_list_downgrades(HyGoal goal)
{
    return list_results(goal, SOLVER_TRANSACTION_DOWNGRADE);
}

/**
 * Return the package upgraded or downgraded by 'pkg'.
 *
 * The returned package has to be freed via hy_package_free().
 */
HyPackage
hy_goal_package_obsoletes(HyGoal goal, HyPackage pkg)
{
    Pool *pool = sack_pool(goal->sack);
    Transaction *trans = goal->trans;
    Id p;

    assert(trans);
    p = transaction_obs_pkg(trans, package_id(pkg));
    assert(p); // todo: handle no upgrades case
    return package_create(pool, p);
}
