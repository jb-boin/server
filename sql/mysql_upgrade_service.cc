/* Copyright (C) 2010-2011 Monty Program Ab & Vladislav Vaintroub

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  mysql_upgrade_service upgrades mysql service on Windows.
  It changes service definition to point to the new mysqld.exe, restarts the 
  server and runs mysql_upgrade
*/

#define DONT_DEFINE_VOID
#include "mariadb.h"
#include <process.h>
#include <my_getopt.h>
#include <my_sys.h>
#include <m_string.h>
#include <mysql_version.h>
#include <winservice.h>

#include <windows.h>
#include <string>

extern int upgrade_config_file(const char *myini_path);

/* We're using version APIs */
#pragma comment(lib, "version")

#define USAGETEXT \
"mysql_upgrade_service.exe  Ver 1.00 for Windows\n" \
"Copyright (C) 2010-2011 Monty Program Ab & Vladislav Vaintroub" \
"This software comes with ABSOLUTELY NO WARRANTY. This is free software,\n" \
"and you are welcome to modify and redistribute it under the GPL v2 license\n" \
"Usage: mysql_upgrade_service.exe [OPTIONS]\n" \
"OPTIONS:"

static char mysqld_path[MAX_PATH];
static char mysqlupgrade_path[MAX_PATH];

static char defaults_file_param[MAX_PATH + 16]; /*--defaults-file=<path> */
static char logfile_path[MAX_PATH];
char my_ini_bck[MAX_PATH];
mysqld_service_properties service_properties;
static char *opt_service;
static SC_HANDLE service;
static SC_HANDLE scm;
HANDLE mysqld_process; // mysqld.exe started for upgrade
DWORD initial_service_state= UINT_MAX; // initial state of the service
HANDLE logfile_handle;

/*
  Startup and shutdown timeouts, in seconds. 
  Maybe,they can be made parameters
*/
static unsigned int startup_timeout= 60;
static unsigned int shutdown_timeout= 60*60;

static struct my_option my_long_options[]=
{
  {"help", '?', "Display this help message and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"service", 'S', "Name of the existing Windows service",
  &opt_service, &opt_service, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};



static my_bool
get_one_option(const struct my_option *opt, const char *, const char *)
{
  DBUG_ENTER("get_one_option");
  switch (opt->id) {
  case '?':
    printf("%s\n", USAGETEXT);
    my_print_help(my_long_options);
    exit(0);
    break;
  }
  DBUG_RETURN(0);
}



static void log(const char *fmt, ...)
{
  va_list args;
  /* Print the error message */
  va_start(args, fmt);
  vfprintf(stdout,fmt, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}


static void die(const char *fmt, ...)
{
  va_list args;
  DBUG_ENTER("die");

  /* Print the error message */
  va_start(args, fmt);

  fprintf(stderr, "FATAL ERROR: ");
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  if (logfile_path[0])
  {
    fprintf(stderr, "Additional information can be found in the log file %s",
      logfile_path);
  }
  va_end(args);
  fputc('\n', stderr);
  fflush(stdout);
  /* Cleanup */

  if (my_ini_bck[0])
  {
    MoveFileEx(my_ini_bck, service_properties.inifile,MOVEFILE_REPLACE_EXISTING);
  }

  /*
    Stop service that we started, if it was not initially running at
    program start.
  */
  if (initial_service_state != UINT_MAX && initial_service_state != SERVICE_RUNNING)
  {
    SERVICE_STATUS service_status;
    ControlService(service, SERVICE_CONTROL_STOP, &service_status);
  }

  if (scm)
    CloseServiceHandle(scm);
  if (service)
    CloseServiceHandle(service);
  /* Stop mysqld.exe, if it was started for upgrade */
  if (mysqld_process)
    TerminateProcess(mysqld_process, 3);
  if (logfile_handle)
    CloseHandle(logfile_handle);
  my_end(0);

  exit(1);
}

#define WRITE_LOG(fmt,...) {\
  char log_buf[1024]; \
  DWORD nbytes; \
  snprintf(log_buf,sizeof(log_buf), fmt, __VA_ARGS__);\
  WriteFile(logfile_handle,log_buf, (DWORD)strlen(log_buf), &nbytes , 0);\
}

/*
  spawn-like function to run subprocesses. 
  We also redirect the full output to the log file.

  Typical usage could be something like
  run_tool(P_NOWAIT, "cmd.exe", "/c" , "echo", "foo", NULL)
  
  @param    wait_flag (P_WAIT or P_NOWAIT)
  @program  program to run

  Rest of the parameters is NULL terminated strings building command line.

  @return intptr containing either process handle, if P_NOWAIT is used
  or return code of the process (if P_WAIT is used)
*/

static intptr_t run_tool(int wait_flag, const char *program,...)
{
  static char cmdline[32*1024];
  char *end;
  va_list args;
  va_start(args, program);
  if (!program)
    die("Invalid call to run_tool");
  end= strxmov(cmdline, "\"", program, "\"", NullS);

  for(;;) 
  {
    char *param= va_arg(args,char *);
    if(!param)
      break;
    end= strxmov(end, " \"", param, "\"", NullS);
  }
  va_end(args);
  
  /* Create output file if not alredy done */
  if (!logfile_handle)
  {
    char tmpdir[FN_REFLEN];
    GetTempPath(FN_REFLEN, tmpdir);
    sprintf_s(logfile_path, "%smysql_upgrade_service.%s.log", tmpdir,
      opt_service);
    SECURITY_ATTRIBUTES attr= {0};
    attr.nLength= sizeof(SECURITY_ATTRIBUTES);
    attr.bInheritHandle=  TRUE;
    logfile_handle= CreateFile(logfile_path, FILE_APPEND_DATA,
      FILE_SHARE_READ|FILE_SHARE_WRITE, &attr, CREATE_ALWAYS, 0, NULL);
    if (logfile_handle == INVALID_HANDLE_VALUE)
    {
      die("Cannot open log file %s, windows error %u", 
        logfile_path, GetLastError());
    }
  }

  WRITE_LOG("Executing %s\r\n", cmdline);

  /* Start child process */
  STARTUPINFO si= {0};
  si.cb= sizeof(si);
  si.hStdInput= GetStdHandle(STD_INPUT_HANDLE);
  si.hStdError= logfile_handle;
  si.hStdOutput= logfile_handle;
  si.dwFlags= STARTF_USESTDHANDLES;
  PROCESS_INFORMATION pi;
  if (!CreateProcess(NULL, cmdline, NULL, 
       NULL, TRUE, NULL, NULL, NULL, &si, &pi))
  {
    die("CreateProcess failed (commandline %s)", cmdline);
  }
  CloseHandle(pi.hThread);

  if (wait_flag == P_NOWAIT)
  {
    /* Do not wait for process to complete, return handle. */
    return (intptr_t)pi.hProcess;
  }

  /* Wait for process to complete. */
  if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0)
  {
    die("WaitForSingleObject() failed");
  }
  DWORD exit_code;
  if (!GetExitCodeProcess(pi.hProcess, &exit_code))
  {
    die("GetExitCodeProcess() failed");
  }
  return (intptr_t)exit_code;
}


void stop_mysqld_service()
{
  DWORD needed;
  SERVICE_STATUS_PROCESS ssp;
  int timeout= shutdown_timeout*1000; 
  for(;;)
  {
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
          (LPBYTE)&ssp, 
          sizeof(SERVICE_STATUS_PROCESS),
          &needed))
    {
      die("QueryServiceStatusEx failed (%u)\n", GetLastError()); 
    }

    /*
      Remember initial state of the service, we will restore it on
      exit.
    */
    if(initial_service_state == UINT_MAX)
      initial_service_state= ssp.dwCurrentState;

    switch(ssp.dwCurrentState)
    {
      case SERVICE_STOPPED:
        return;
      case SERVICE_RUNNING:
        if(!ControlService(service, SERVICE_CONTROL_STOP, 
             (SERVICE_STATUS *)&ssp))
            die("ControlService failed, error %u\n", GetLastError());
      case SERVICE_START_PENDING:
      case SERVICE_STOP_PENDING:
        if(timeout < 0)
          die("Service does not stop after %d seconds timeout",shutdown_timeout);
        Sleep(100);
        timeout -= 100;
        break;
      default:
        die("Unexpected service state %d",ssp.dwCurrentState);
    }
  }
}


/* 
  Shutdown mysql server. Not using mysqladmin, since 
  our --skip-grant-tables do not work anymore after mysql_upgrade
  that does "flush privileges". Instead, the shutdown event  is set.
*/
#define OPEN_EVENT_RETRY_SLEEP_MS 100
#define OPEN_EVENT_MAX_RETRIES 50

void initiate_mysqld_shutdown()
{
  char event_name[32];
  DWORD pid= GetProcessId(mysqld_process);
  sprintf_s(event_name, "MySQLShutdown%d", pid);

  HANDLE shutdown_handle;
  for (int i= 0;; i++)
  {
    shutdown_handle= OpenEvent(EVENT_MODIFY_STATE, FALSE, event_name);
    if(shutdown_handle != nullptr || i == OPEN_EVENT_MAX_RETRIES)
      break;
    if (WaitForSingleObject(mysqld_process, OPEN_EVENT_RETRY_SLEEP_MS) !=
        WAIT_TIMEOUT)
    {
      die("server process exited before shutdown event was created");
      break;
    }
  }
  if (!shutdown_handle)
  {
    die("OpenEvent() failed for shutdown event");
  }

  if(!SetEvent(shutdown_handle))
  {
    die("SetEvent() failed");
  }
}

static void get_service_config()
{
  scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
  if (!scm)
    die("OpenSCManager failed with %u", GetLastError());
  service = OpenService(scm, opt_service, SERVICE_ALL_ACCESS);
  if (!service)
    die("OpenService failed with %u", GetLastError());

  alignas(QUERY_SERVICE_CONFIGW) BYTE config_buffer[8 * 1024];
  LPQUERY_SERVICE_CONFIGW config = (LPQUERY_SERVICE_CONFIGW)config_buffer;
  DWORD size = sizeof(config_buffer);
  DWORD needed;
  if (!QueryServiceConfigW(service, config, size, &needed))
    die("QueryServiceConfig failed with %u", GetLastError());

  if (get_mysql_service_properties(config->lpBinaryPathName, &service_properties))
  {
    die("Not a valid MySQL service");
  }

  int my_major = MYSQL_VERSION_ID / 10000;
  int my_minor = (MYSQL_VERSION_ID % 10000) / 100;
  int my_patch = MYSQL_VERSION_ID % 100;

  if (my_major < service_properties.version_major ||
    (my_major == service_properties.version_major && my_minor < service_properties.version_minor))
  {
    die("Can not downgrade, the service is currently running as version %d.%d.%d"
      ", my version is %d.%d.%d", service_properties.version_major, service_properties.version_minor,
      service_properties.version_patch, my_major, my_minor, my_patch);
  }
  if (service_properties.inifile[0] == 0)
  {
    /*
      Weird case, no --defaults-file in service definition, need to create one.
    */
    sprintf_s(service_properties.inifile, MAX_PATH, "%s\\my.ini", service_properties.datadir);
  }
  sprintf(defaults_file_param, "--defaults-file=%s", service_properties.inifile);
}
/*
  Change service configuration (binPath) to point to mysqld from 
  this installation.
*/
static void change_service_config()
{
  char buf[MAX_PATH];
  char commandline[3 * MAX_PATH + 19];
  int i;

  /*
    Write datadir to my.ini, after converting  backslashes to 
    unix style slashes.
  */
  if (service_properties.datadir[0])
  {
    strcpy_s(buf, MAX_PATH, service_properties.datadir);
    for (i= 0; buf[i]; i++)
    {
      if (buf[i] == '\\')
        buf[i]= '/';
    }
    WritePrivateProfileString("mysqld", "datadir", buf,
                              service_properties.inifile);
  }

  /*
    Remove basedir from defaults file, otherwise the service wont come up in 
    the new version, and will complain about mismatched message file.
  */
  WritePrivateProfileString("mysqld", "basedir",NULL, service_properties.inifile);

  sprintf(defaults_file_param,"--defaults-file=%s", service_properties.inifile);
  sprintf_s(commandline, "\"%s\" \"%s\" \"%s\"", mysqld_path, 
   defaults_file_param, opt_service);
  if (!ChangeServiceConfig(service, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, 
         SERVICE_NO_CHANGE, commandline, NULL, NULL, NULL, NULL, NULL, NULL))
  {
    die("ChangeServiceConfig failed with %u", GetLastError());
  }

}

/**
  Waits until starting server can be connected to, via given named pipe, with timeout
  Dies if either server process exited meanwhile, or when timeout was exceeded.
*/
static void wait_for_server_startup(HANDLE process, const char *named_pipe, DWORD timeout_sec)
{
  unsigned long long end_time= GetTickCount64() + 1000ULL*timeout_sec;
  for (;;)
  {
    if (WaitNamedPipe(named_pipe, 0))
      return;

    if (GetTickCount64() >= end_time)
      die("Server did not startup after %lu seconds", timeout_sec);

    if (WaitForSingleObject(process, 100) != WAIT_TIMEOUT)
      die("Server did not start");
  }
}


int main(int argc, char **argv)
{
  int error;
  MY_INIT(argv[0]);
  char bindir[FN_REFLEN];
  char *p;

  /* Parse options */
  if ((error= handle_options(&argc, &argv, my_long_options, get_one_option)))
    die("");
  if (!opt_service)
    die("--service=# parameter is mandatory");
 
 /*
    Get full path to mysqld, we need it when changing service configuration.
    Assume mysqld.exe in the same directory as this program.
    mysql_upgrade.exe is either in the same directory, or pointed to by
    MARIADB_UPGRADE_EXE environment variable (in case of MTR running it)
  */
  GetModuleFileName(NULL, bindir, FN_REFLEN);
  p= strrchr(bindir, FN_LIBCHAR);
  if(p)
  {
    *p= 0;
  }
  sprintf_s(mysqld_path, "%s\\mysqld.exe", bindir);
  sprintf_s(mysqlupgrade_path, "%s\\mysql_upgrade.exe", bindir);

  if (access(mysqld_path, 0))
    die("File %s does not exist", mysqld_path);
  if (access(mysqlupgrade_path, 0))
  {
    /* Try to get path from environment variable, set by MTR */
    char *alt_mysqlupgrade_path= getenv("MARIADB_UPGRADE_EXE");
    if (alt_mysqlupgrade_path)
      sprintf_s(mysqlupgrade_path, "%s", alt_mysqlupgrade_path);
  }
  if (access(mysqlupgrade_path, 0))
    die("File %s does not exist", mysqld_path);

  /*
    Messages written on stdout should not be buffered,  GUI upgrade program 
    reads them from pipe and uses as progress indicator.
  */
  setvbuf(stdout, NULL, _IONBF, 0);
  int phase = 0;
  int max_phases=10;
  get_service_config();

  bool my_ini_exists;
  bool old_mysqld_exe_exists;

  log("Phase %d/%d: Stopping service", ++phase,max_phases);
  stop_mysqld_service();

  my_ini_exists = (GetFileAttributes(service_properties.inifile) != INVALID_FILE_ATTRIBUTES);
  if (!my_ini_exists)
  {
    HANDLE h = CreateFile(service_properties.inifile, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
      0, CREATE_NEW, 0 ,0);
    if (h != INVALID_HANDLE_VALUE)
    {
      CloseHandle(h);
    }
    else if (GetLastError() != ERROR_FILE_EXISTS)
    {
      die("Can't create ini file %s, last error %u", service_properties.inifile, GetLastError());
    }
  }

  old_mysqld_exe_exists= (GetFileAttributes(service_properties.mysqld_exe) !=
                          INVALID_FILE_ATTRIBUTES);
  bool do_start_stop_server = old_mysqld_exe_exists && initial_service_state != SERVICE_RUNNING;

  log("Phase %d/%d: Start and stop server in the old version, to avoid crash recovery %s", ++phase, max_phases,
    do_start_stop_server?",this can take some time":"(skipped)");

  char socket_param[FN_REFLEN];
  sprintf_s(socket_param, "--socket=mysql_upgrade_service_%u",
    GetCurrentProcessId());

  DWORD start_duration_ms = 0;

  char pipe_name[64];
  snprintf(pipe_name, sizeof(pipe_name),
           "\\\\.\\pipe\\mysql_upgrade_service_%lu", GetCurrentProcessId());

  if (do_start_stop_server)
  {
    /* Start/stop server with  --loose-innodb-fast-shutdown=1 */
    mysqld_process = (HANDLE)run_tool(P_NOWAIT, service_properties.mysqld_exe,
      defaults_file_param, "--loose-innodb-fast-shutdown=1", "--skip-networking",
      "--enable-named-pipe", socket_param, "--skip-slave-start", NULL);

    if (mysqld_process == INVALID_HANDLE_VALUE)
    {
      die("Cannot start mysqld.exe process, last error =%u", GetLastError());
    }
    wait_for_server_startup(mysqld_process, pipe_name, startup_timeout);
    // Server started, shut it down.
    initiate_mysqld_shutdown();
    if (WaitForSingleObject((HANDLE)mysqld_process, shutdown_timeout * 1000) != WAIT_OBJECT_0)
    {
       die("Could not shutdown server");
    }
    DWORD exit_code;
    if (!GetExitCodeProcess((HANDLE)mysqld_process, &exit_code))
    {
      die("Could not get server's exit code");
    }
    if (exit_code)
    {
      die("Could not get successfully shutdown server (exit code %u)",exit_code);
    }
    CloseHandle(mysqld_process);
  }

  log("Phase %d/%d: Fixing server config file%s", ++phase, max_phases,
      my_ini_exists ? "" : "(skipped)");
  snprintf(my_ini_bck, sizeof(my_ini_bck), "%s.BCK",
           service_properties.inifile);
  CopyFile(service_properties.inifile, my_ini_bck, FALSE);
  upgrade_config_file(service_properties.inifile);

  /* 
    Start mysqld.exe as non-service skipping privileges (so we do not 
    care about the password). But disable networking and enable pipe 
    for communication, for security reasons.
  */

  log("Phase %d/%d: Starting mysqld for upgrade",++phase,max_phases);
  mysqld_process= (HANDLE)run_tool(P_NOWAIT, mysqld_path,
    defaults_file_param, "--skip-networking",  "--skip-grant-tables", 
    "--enable-named-pipe",  socket_param,"--skip-slave-start", NULL);

  if (mysqld_process == INVALID_HANDLE_VALUE)
  {
    die("Cannot start mysqld.exe process, errno=%d", errno);
  }

  log("Phase %d/%d: Waiting for startup to complete",++phase,max_phases);
  wait_for_server_startup(mysqld_process, pipe_name, startup_timeout);

  log("Phase %d/%d: Running mysql_upgrade",++phase,max_phases);
  int upgrade_err= (int) run_tool(P_WAIT,  mysqlupgrade_path, 
    "--protocol=pipe", "--force",  socket_param,
    NULL);

  if (upgrade_err)
    die("mysql_upgrade failed with error code %d\n", upgrade_err);

  log("Phase %d/%d: Changing service configuration", ++phase, max_phases);
  change_service_config();

  log("Phase %d/%d: Initiating server shutdown",++phase, max_phases);
  initiate_mysqld_shutdown();

  log("Phase %d/%d: Waiting for shutdown to complete",++phase, max_phases);
  if (WaitForSingleObject(mysqld_process, shutdown_timeout*1000)
      != WAIT_OBJECT_0)
  {
    /* Shutdown takes too long */
    die("mysqld does not shutdown.");
  }
  CloseHandle(mysqld_process);
  mysqld_process= NULL;

  log("Phase %d/%d: Starting service%s",++phase,max_phases,
    (initial_service_state == SERVICE_RUNNING)?"":" (skipped)");
  if (initial_service_state == SERVICE_RUNNING)
  {
    StartService(service, NULL, NULL);
  }

  log("Service '%s' successfully upgraded.\nLog file is written to %s",
    opt_service, logfile_path);
  CloseServiceHandle(service);
  CloseServiceHandle(scm);
  if (logfile_handle)
    CloseHandle(logfile_handle);
  if(my_ini_bck[0])
  {
    DeleteFile(my_ini_bck);
  }
  my_end(0);
  exit(0);
}
