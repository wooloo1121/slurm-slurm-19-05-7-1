/*****************************************************************************\
 *  backfill_wrapper.c - plugin for Slurm backfill scheduler.
 *  Operates like FIFO, but backfill scheduler daemon will explicitly modify
 *  the priority of jobs as needed to achieve backfill scheduling.
 *****************************************************************************
 *  Copyright (C) 2003 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Jay Windley <jwindley@lnxi.com>, Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
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
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
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

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "slurm/slurm_errno.h"

#include "src/common/plugin.h"
#include "src/common/log.h"
#include "src/common/slurm_priority.h"
#include "src/common/macros.h"
#include "src/slurmctld/slurmctld.h"
#include "backfill.h"

#include <Python.h>
#include <dlfcn.h>

const char		plugin_name[]	= "Slurm Backfill Scheduler plugin";
const char		plugin_type[]	= "sched/backfill";
const uint32_t		plugin_version	= SLURM_VERSION_NUMBER;

static pthread_t backfill_thread = 0;
static pthread_mutex_t thread_flag_mutex = PTHREAD_MUTEX_INITIALIZER;

int init( void )
{
	if (slurmctld_config.scheduling_disabled)
		return SLURM_SUCCESS;

	sched_verbose("Backfill scheduler plugin loaded");

	slurm_mutex_lock( &thread_flag_mutex );
	if ( backfill_thread ) {
		debug2( "Backfill thread already running, not starting "
			"another" );
		slurm_mutex_unlock( &thread_flag_mutex );
		return SLURM_ERROR;
	}

	/* since we do a join on this later we don't make it detached */
	slurm_thread_create(&backfill_thread, backfill_agent, NULL);

	slurm_mutex_unlock( &thread_flag_mutex );

        dlopen("libpython2.7.so", RTLD_LAZY | RTLD_GLOBAL);
        Py_Initialize();

	return SLURM_SUCCESS;
}

void fini( void )
{
	slurm_mutex_lock( &thread_flag_mutex );
	if ( backfill_thread ) {
		verbose( "Backfill scheduler plugin shutting down" );
		stop_backfill_agent();
		pthread_join(backfill_thread, NULL);
		backfill_thread = 0;
                Py_Finalize();
	}
	slurm_mutex_unlock( &thread_flag_mutex );
}

int slurm_sched_p_reconfig( void )
{
	backfill_reconfig();
	return SLURM_SUCCESS;
}

uint32_t slurm_sched_p_initial_priority(uint32_t last_prio,
					struct job_record *job_ptr)
{
    PyObject *pName, *pModule, *pFunc;
    PyObject *pArgs, *pValue, *pValue_array, *pReturn;

    PyRun_SimpleString("import sys\nsys.path.append('/home/slurm/')");
    
    pName = PyString_FromString("predict_func_v1");
    /* Error checking of pName left out */
    pModule = PyImport_Import(pName);
    Py_DECREF(pName);

    if (pModule != NULL) {
        pFunc = PyObject_GetAttrString(pModule, "priority");
        /* pFunc is a new reference */
        if (pFunc && PyCallable_Check(pFunc)) {
            if (job_ptr->details && job_ptr->details->argv) {
                pValue = PyString_FromString(job_ptr->details->argv[0]);
            }
            else {
                Py_DECREF(pFunc);
                Py_DECREF(pModule);
                info("no script path information");
                return priority_g_set(last_prio, job_ptr);   
            }

            if (!pValue) {
                Py_DECREF(pFunc);
                Py_DECREF(pModule);
                info("Cannot convert job script path");
                return priority_g_set(last_prio, job_ptr);
            }
            pArgs = PyTuple_New(1);
            PyTuple_SetItem(pArgs, 0, pValue);
            pReturn = PyObject_CallObject(pFunc, pArgs);
            Py_DECREF(pArgs);
            if (pReturn != NULL) {
                if (PyInt_AsLong(pReturn) == 1)
                {
                        Py_DECREF(pReturn);
                        Py_DECREF(pFunc);
                        Py_DECREF(pModule);
			info("Job %u delayed! last_prio %u reduced to %u.", job_ptr->job_id, last_prio, last_prio / 2);
                	return priority_g_set(last_prio / 2, job_ptr);
                }
                else
                {
                        Py_DECREF(pReturn);
                        Py_DECREF(pFunc);
                        Py_DECREF(pModule);
                        info("Job %u not delayed! last_prio %u.", job_ptr->job_id, last_prio);
                        return priority_g_set(last_prio, job_ptr);
	        }
	    }
            else {
                Py_DECREF(pFunc);
                Py_DECREF(pModule);
                info("Call failed");
                return priority_g_set(last_prio, job_ptr);
            }
        }
        else {
            info("Cannot find function priority");
        }
        Py_XDECREF(pFunc);
        Py_DECREF(pModule);
    }
    else {
        info("Failed to load predict_func");
    }
    return priority_g_set(last_prio, job_ptr);

}
