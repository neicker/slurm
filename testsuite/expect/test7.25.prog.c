/*****************************************************************************\
 *  prog7.25.prog.c - SPANK plugin for testing purposes
 *****************************************************************************
 *  Copyright (C) 2023 Forschungszentrum Juelich GmbH
 *  Written by Norbert Eicker <n.eicker@fz-juelich.de
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/
#include <stdlib.h>

#include <slurm/spank.h>

/*
 * All spank plugins must define this macro for the Slurm plugin loader.
 */
SPANK_PLUGIN(test_suite, 1);

static int enable_plugin = 0;
static int enable_privileged = 0;

static int _test_opt_process(int val, const char *optarg, int remote);

/*
 *  Provide a --test_suite option to srun:
 */
struct spank_option spank_options[] =
{
	{ "test_suite",
	  "[enable_priv]",
	  "Component of slurm test suite",
	  2,
	  0,
	  _test_opt_process
	},
	SPANK_OPTIONS_TABLE_END
};

static int _test_opt_process(int val, const char *optarg, int remote)
{
	enable_plugin = 1;

	if (optarg && optarg[0]) enable_privileged = 1;

	return (0);
}

/* Called from slurmd only */
int slurm_spank_task_init(spank_t sp, int ac, char **av)
{
	const char *argv[] = { "env", "TASK_INIT=1", NULL };
	spank_err_t err;

	if (!enable_plugin) return (0);

	err = spank_prepend_task_argv(sp, 2, argv);

	if (err != ESPANK_SUCCESS) {
		slurm_info("pre-pending 'env TASK_INIT=1' failed: %d\n", err);
		return (-1);
	}

	return (0);
}

/* Called from slurmd only */
int slurm_spank_task_init_privileged(spank_t sp, int ac, char **av)
{
	const char *argv[] = { "env", "TASK_PRIV=1", NULL };
	spank_err_t err;

	if (!enable_privileged) return (0);

	err = spank_prepend_task_argv(sp, 2, argv);

	if (err != ESPANK_SUCCESS) {
		slurm_info("pre-pending 'env TASK_PRIV=1' failed: %d\n", err);
		return (-1);
	}

	return (0);
}
