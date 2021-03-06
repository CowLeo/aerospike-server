/*
 * as.c
 *
 * Copyright (C) 2008-2015 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>
#include <sys/stat.h>

#include "citrusleaf/alloc.h"

#include "ai.h"
#include "fault.h"
#include "jem.h"
#include "util.h"

#include "base/asm.h"
#include "base/batch.h"
#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/json_init.h"
#include "base/monitor.h"
#include "base/scan.h"
#include "base/secondary_index.h"
#include "base/security.h"
#include "base/system_metadata.h"
#include "base/thr_batch.h"
#include "base/thr_info.h"
#include "base/thr_proxy.h"
#include "base/thr_sindex.h"
#include "base/thr_tsvc.h"
#include "base/thr_write.h"
#include "base/udf_rw.h"
#include "base/xdr_serverside.h"
#include "fabric/fabric.h"
#include "fabric/hb.h"
#include "fabric/migrate.h"
#include "fabric/paxos.h"
#include "storage/storage.h"


//==========================================================
// Constants.
//

// String constants in version.c, generated by make.
extern const char aerospike_build_type[];
extern const char aerospike_build_id[];

// Command line options for the Aerospike server.
const struct option cmd_opts[] = {
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'v' },
		{ "config-file", required_argument, 0, 'f' },
		{ "foreground", no_argument, 0, 'd' },
		{ "fgdaemon", no_argument, 0, 'F' },
		{ "cold-start", no_argument, 0, 'c' },
		{ "instance", required_argument, 0, 'n' },
		{ 0, 0, 0, 0 }
};

const char HELP[] =
		"\n"
		"Aerospike server installation installs the script /etc/init.d/aerospike which\n"
		"is normally used to start and stop the server. The script is also found as\n"
		"as/etc/init-script in the source tree.\n"
		"\n"
		"asd informative command-line options:\n"
		"\n"
		"--help"
		"\n"
		"Print this message and exit.\n"
		"\n"
		"--version"
		"\n"
		"Print edition and build version information and exit.\n"
		"\n"
		"asd runtime command-line options:\n"
		"\n"
		"--config-file <file>"
		"\n"
		"Specify the location of the Aerospike server config file. If this option is not\n"
		"specified, the default location /etc/aerospike/aerospike.conf is used.\n"
		"\n"
		"--foreground"
		"\n"
		"Specify that Aerospike not be daemonized. This is useful for running Aerospike\n"
		"in gdb. Alternatively, add 'run-as-daemon false' in the service context of the\n"
		"Aerospike config file.\n"
		"\n"
		"--fgdaemon"
		"\n"
		"Specify that Aerospike is to be run as a \"new-style\" (foreground) daemon. This\n"
		"is useful for running Aerospike under systemd or Docker.\n"
		"\n"
		"--cold-start"
		"\n"
		"(Enterprise edition only.) At startup, force the Aerospike server to read all\n"
		"records from storage devices to rebuild the index.\n"
		"\n"
		"--instance <0-15>"
		"\n"
		"(Enterprise edition only.) If running multiple instances of Aerospike on one\n"
		"machine (not recommended), each instance must be uniquely designated via this\n"
		"option.\n"
		;

const char USAGE[] =
		"\n"
		"asd informative command-line options:\n"
		"[--help]\n"
		"[--version]\n"
		"\n"
		"asd runtime command-line options:\n"
		"[--config-file <file>] "
		"[--foreground] "
		"[--fgdaemon] "
		"[--cold-start] "
		"[--instance <0-15>]\n"
		;

const char DEFAULT_CONFIG_FILE[] = "/etc/aerospike/aerospike.conf";


//==========================================================
// Globals.
//

// The mutex that the main function deadlocks on after starting the service.
pthread_mutex_t g_NONSTOP;
bool g_startup_complete = false;
bool g_shutdown_started = false;


//==========================================================
// Forward declarations.
//

// signal.c, thr_demarshal.c and thr_nsup.c don't have header files.
extern void as_signal_setup();
extern void as_demarshal_start();
extern void as_nsup_start();


//==========================================================
// Local helpers.
//

static void
write_pidfile(char* pidfile)
{
	if (! pidfile) {
		// If there's no pid file specified in the config file, just move on.
		return;
	}

	// Note - the directory the pid file is in must already exist.

	remove(pidfile);

	int pid_fd = open(pidfile, O_CREAT | O_RDWR,
			S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);

	if (pid_fd < 0) {
		cf_crash_nostack(AS_AS, "failed to open pid file %s: %s", pidfile,
				cf_strerror(errno));
	}

	char pidstr[16];
	sprintf(pidstr, "%u\n", (uint32_t)getpid());

	// If we can't access this resource, just log a warning and continue -
	// it is not critical to the process.
	if (-1 == write(pid_fd, pidstr, strlen(pidstr))) {
		cf_warning(AS_AS, "failed write to pid file %s: %s", pidfile,
				cf_strerror(errno));
	}

	close(pid_fd);
}

static void
validate_directory(const char* path, const char* log_tag)
{
	struct stat buf;

	if (stat(path, &buf) != 0) {
		cf_crash_nostack(AS_AS, "%s directory '%s' is not set up properly: %s",
				log_tag, path, cf_strerror(errno));
	}
	else if (! S_ISDIR(buf.st_mode)) {
		cf_crash_nostack(AS_AS, "%s directory '%s' is not set up properly: Not a directory",
				log_tag, path);
	}
}

static void
validate_smd_directory()
{
	size_t len = strlen(g_config.work_directory);
	const char SMD_DIR_NAME[] = "/smd";
	char smd_path[len + sizeof(SMD_DIR_NAME)];

	strcpy(smd_path, g_config.work_directory);
	strcpy(smd_path + len, SMD_DIR_NAME);
	validate_directory(smd_path, "system metadata");
}


//==========================================================
// Aerospike server entry point.
//

int
main(int argc, char **argv)
{
#ifdef USE_ASM
	as_mallocation_t asm_array[MAX_NUM_MALLOCATIONS];

	// Zero-out the statically-allocated array of memory allocation locations.
	memset(asm_array, 0, sizeof(asm_array));

	// Set the ASMalloc callback user data.
	g_my_cb_udata = asm_array;

	// This must come first to allow initialization of the ASMalloc library.
	asm_init();
#endif // defined(USE_ASM)

#ifdef USE_JEM
	// Initialize the JEMalloc interface.
	jem_init(true);
#endif

	// Initialize ref-counting system.
	cf_rc_init(NULL);

	// Initialize fault management framework.
	cf_fault_init();

	// Setup signal handlers.
	as_signal_setup();

	// Initialize the Jansson JSON API.
	as_json_init();

	int i;
	int cmd_optidx;
	const char *config_file = DEFAULT_CONFIG_FILE;
	bool run_in_foreground = false;
	bool new_style_daemon = false;
	bool cold_start_cmd = false;
	uint32_t instance = 0;

	// Parse command line options.
	while (-1 != (i = getopt_long(argc, argv, "", cmd_opts, &cmd_optidx))) {
		switch (i) {
		case 'h':
			// printf() since we want stdout and don't want cf_fault's prefix.
			printf("%s\n", HELP);
			return 0;
		case 'v':
			// printf() since we want stdout and don't want cf_fault's prefix.
			printf("%s build %s\n", aerospike_build_type, aerospike_build_id);
			return 0;
		case 'f':
			config_file = cf_strdup(optarg);
			cf_assert(config_file, AS_AS, CF_CRITICAL, "config filename cf_strdup failed");
			break;
		case 'F':
			// As a "new-style" daemon(*), asd runs in the foreground and
			// ignores the following configuration items:
			//  - user ('user')
			//	- group ('group')
			//  - PID file ('pidfile')
			//
			// If ignoring configuration items, or if the 'console' sink is not
			// specified, warnings will appear in stderr.
			//
			// (*) http://0pointer.de/public/systemd-man/daemon.html#New-Style%20Daemons
			run_in_foreground = true;
			new_style_daemon = true;
			break;
		case 'd':
			run_in_foreground = true;
			break;
		case 'c':
			cold_start_cmd = true;
			break;
		case 'n':
			instance = (uint32_t)strtol(optarg, NULL, 0);
			break;
		default:
			// fprintf() since we don't want cf_fault's prefix.
			fprintf(stderr, "%s\n", USAGE);
			return 1;
		}
	}

	// Set all fields in the global runtime configuration instance. This parses
	// the configuration file, and creates as_namespace objects. (Return value
	// is a shortcut pointer to the global runtime configuration instance.)
	as_config *c = as_config_init(config_file);

#ifdef USE_ASM
	g_asm_hook_enabled = g_asm_cb_enabled = c->asmalloc_enabled;

	long initial_tid = syscall(SYS_gettid);
#endif

#ifdef MEM_COUNT
	// [Note: This should ideally be at the very start of the "main()" function,
	//        but we need to wait until after the config file has been parsed in
	//        order to support run-time configurability.]
	mem_count_init(c->memory_accounting ? MEM_COUNT_ENABLE : MEM_COUNT_DISABLE);
#endif

	// Perform privilege separation as necessary. If configured user & group
	// don't have root privileges, all resources created or reopened past this
	// point must be set up so that they are accessible without root privileges.
	// If not, the process will self-terminate with (hopefully!) a log message
	// indicating which resource is not set up properly.
	if (0 != c->uid && 0 == geteuid()) {
		if (! new_style_daemon) {
			// To see this log, change NO_SINKS_LIMIT in fault.c:
			cf_info(AS_AS, "privsep to %d %d", c->uid, c->gid);
			cf_process_privsep(c->uid, c->gid);
		}
		else {
			cf_warning(AS_AS, "will not do privsep in new-style daemon mode");
		}
	}

	//
	// All resources such as files, devices, and shared memory must be created
	// or reopened below this line! (The configuration file is the only thing
	// that must be opened above, in order to parse the user & group.)
	//==========================================================================

	// A "new-style" daemon expects console logging to be configured. (If not,
	// log messages won't be seen via the standard path.)
	if (new_style_daemon) {
		if (! cf_fault_console_is_held()) {
			cf_warning(AS_AS, "in new-style daemon mode, console logging is not configured");
		}
	}

	// Activate log sinks. Up to this point, 'cf_' log output goes to stderr,
	// filtered according to NO_SINKS_LIMIT in fault.c. After this point, 'cf_'
	// log output will appear in all log file sinks specified in configuration,
	// with specified filtering. If console sink is specified in configuration,
	// 'cf_' log output will continue going to stderr, but filtering will switch
	// from NO_SINKS_LIMIT to that specified in console sink configuration.
	if (0 != cf_fault_sink_activate_all_held()) {
		// Specifics of failure are logged in cf_fault_sink_activate_all_held().
		cf_crash_nostack(AS_AS, "can't open log sink(s)");
	}

	// Daemonize asd if specified. After daemonization, output to stderr will no
	// longer appear in terminal. Instead, check /tmp/aerospike-console.<pid>
	// for console output.
	if (! run_in_foreground && c->run_as_daemon) {
		// Don't close any open files when daemonizing. At this point only log
		// sink files are open - instruct cf_process_daemonize() to ignore them.
		int open_fds[CF_FAULT_SINKS_MAX];
		int num_open_fds = cf_fault_sink_get_fd_list(open_fds);

		cf_process_daemonize(open_fds, num_open_fds);
	}

#ifdef USE_ASM
	// Log the main thread's Linux Task ID (pre- and post-fork) to the console.
	fprintf(stderr, "Initial main thread tid: %lu\n", initial_tid);

	if (! run_in_foreground && c->run_as_daemon) {
		fprintf(stderr, "Post-daemonize main thread tid: %lu\n",
				syscall(SYS_gettid));
	}
#endif

	// Log which build this is - should be the first line in the log file.
	cf_info(AS_AS, "<><><><><><><><><><>  %s build %s  <><><><><><><><><><>",
			aerospike_build_type, aerospike_build_id);

	// Includes echoing the configuration file to log.
	as_config_post_process(c, config_file);

	// Make one more pass for XDR-related config and crash if needed.
	// TODO : XDR config parsing should be merged with main config parsing.
	xdr_conf_init(config_file);

	// If we allocated a non-default config file name, free it.
	if (config_file != DEFAULT_CONFIG_FILE) {
		cf_free((void*)config_file);
	}

	// Write the pid file, if specified.
	if (! new_style_daemon) {
		write_pidfile(c->pidfile);
	}
	else {
		if (c->pidfile) {
			cf_warning(AS_AS, "will not write PID file in new-style daemon mode");
		}
	}

	// Check that required directories are set up properly.
	validate_directory(c->work_directory, "work");
	validate_directory(c->mod_lua.system_path, "Lua system");
	validate_directory(c->mod_lua.user_path, "Lua user");
	validate_smd_directory();

	// Initialize subsystems. At this point we're allocating local resources,
	// starting worker threads, etc. (But no communication with other server
	// nodes or clients yet.)

	as_smd_init();				// System Metadata first - others depend on it
	ai_init();					// before as_storage_init() populates indexes
	as_sindex_thr_init();		// defrag secondary index (ok during population)

	// Initialize namespaces. Each namespace decides here whether it will do a
	// warm or cold start. Index arenas, partition structures and index tree
	// structures are initialized. Secondary index system metadata is restored.
	as_namespaces_init(cold_start_cmd, instance);

	// Initialize the storage system. For cold starts, this includes reading
	// all the objects off the drives. This may block for a long time. The
	// defrag subsystem starts operating at the end of this call.
	as_storage_init();

	// Populate all secondary indexes. This may block for a long time.
	as_sindex_boot_populateall();

	cf_info(AS_AS, "initializing services...");

	as_netio_init();
	as_security_init();			// security features
	as_tsvc_init();				// all transaction handling
	as_hb_init();				// inter-node heartbeat
	as_fabric_init();			// inter-node communications
	as_info_init();				// info transaction handling
	as_paxos_init();			// cluster consensus algorithm
	as_migrate_init();			// move data between nodes
	as_proxy_init();			// do work on behalf of others
	as_write_init();			// write service
	as_query_init();			// query transaction handling
	as_udf_rw_init();			// apply user-defined functions
	as_scan_init();				// scan a namespace or set
	as_batch_init();			// batch transaction handling
	as_batch_direct_init();		// low priority transaction handling        
	as_xdr_init();				// cross data-center replication
	as_mon_init();				// monitor

	// Wait for enough available storage. We've been defragging all along, but
	// here we wait until it's enough. This may block for a long time.
	as_storage_wait_for_defrag();

	// Start subsystems. At this point we may begin communicating with other
	// cluster nodes, and ultimately with clients.

	as_smd_start(c->smd);		// enables receiving paxos state change events
	as_fabric_start();			// may send & receive fabric messages
	as_xdr_start();				// XDR should start before it joins other nodes
	as_hb_start();				// start inter-node heatbeat
	as_paxos_start();			// blocks until cluster membership is obtained
	as_nsup_start();			// may send delete transactions to other nodes
	as_demarshal_start();		// server will now receive client transactions
	as_info_port_start();		// server will now receive info transactions
	info_debug_ticker_start();	// only after everything else is started

	// Log a service-ready message.
	cf_info(AS_AS, "service ready: soon there will be cake!");

	//--------------------------------------------
	// Startup is done. This thread will now wait
	// quietly for a shutdown signal.
	//

	// Stop this thread from finishing. Intentionally deadlocking on a mutex is
	// a remarkably efficient way to do this.
	pthread_mutex_init(&g_NONSTOP, NULL);
	pthread_mutex_lock(&g_NONSTOP);
	g_startup_complete = true;
	pthread_mutex_lock(&g_NONSTOP);

	// When the service is running, you are here (deadlocked) - the signals that
	// stop the service (yes, these signals always occur in this thread) will
	// unlock the mutex, allowing us to continue.

	g_shutdown_started = true;
	pthread_mutex_unlock(&g_NONSTOP);
	pthread_mutex_destroy(&g_NONSTOP);

	//--------------------------------------------
	// Received a shutdown signal.
	//

	as_storage_shutdown();
	as_xdr_shutdown();
	as_smd_shutdown(c->smd);

	cf_info(AS_AS, "finished clean shutdown - exiting");

	// If shutdown was totally clean (all threads joined) we could just return,
	// but for now we exit to make sure all threads die.
#ifdef DOPROFILE
	exit(0); // exit(0) so profile build actually dumps gmon.out
#else
	_exit(0);
#endif

	return 0;
}
