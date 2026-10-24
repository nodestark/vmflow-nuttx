/****************************************************************************
 * firmware/apps/mdb/mdb_engine.h
 *
 * MDB cashless engine: the task that owns /dev/mdb0.
 *
 ****************************************************************************/

#ifndef __VMFLOW_APPS_MDB_MDB_ENGINE_H
#define __VMFLOW_APPS_MDB_MDB_ENGINE_H

#include <stdbool.h>

int  mdb_engine_run(int argc, FAR char *argv[]);
int  mdb_engine_start(void);
bool mdb_engine_running(void);
void mdb_engine_status(void);

#endif /* __VMFLOW_APPS_MDB_MDB_ENGINE_H */
