/*
  MIT License

  Copyright (c) 2018 qbsa

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>
#include <stdio.h>
#include <conio.h>
#include <stdlib.h>
#include <time.h>

// Registry constants
#define MAX_KEY_LENGTH 255
#define MAX_VALUE_NAME 16383

#define REG_ROOT HKEY_LOCAL_MACHINE
#define REG_KEY "SYSTEM\\CurrentControlSet\\Control\\Class"
#define REG_MAX_LEVEL 1
#define REG_VALUE_NAME "EnableUlps"
#define REG_NEW_VALUE 0

// Error handling macro
#define Q(EXPR) if(sc = (EXPR), sc != ERROR_SUCCESS) break;
#define Q_IF_ERRSC() if(sc != ERROR_SUCCESS) break;
#define Q_LAST_IF(...) if(__VA_ARGS__) { sc = GetLastError(); break; }

// Service related functions & macros
#define SERVICE_NAME "ulps"
#define SERVICE_DISPLAY_NAME "EnableUlps Watcher Service"

SERVICE_STATUS          gSvcStatus;
SERVICE_STATUS_HANDLE   gSvcStatusHandle;
HANDLE                  ghStopEvent = NULL;

LONG SvcInstall(char * name);
LONG SvcUninstall(char * name);
VOID WINAPI SvcCtrlHandler( DWORD );
VOID WINAPI SvcMain( DWORD, LPTSTR * );
VOID ReportSvcStatus( DWORD, DWORD, DWORD );
VOID SvcRun( DWORD, LPTSTR *, BOOL);
LONG ElevateIfNeeded();

void PrintError(LONG);
BOOL CloseAndInvalidateHandle(PHANDLE);
LONG WaitForRegistryChanges();
LONG LookupForChanges(HKEY, int);

void wlog(char *);

BOOL WINAPI ConsoleCtrlHandlerRoutine(DWORD dwCtrlType)
{
  if (ghStopEvent != NULL) {
    SetEvent(ghStopEvent);
    return TRUE;
  }
  return FALSE;
}

int main(int argc, char * argv[])
{
  LONG sc = ERROR_SUCCESS;
  BOOL serviceMode = 0;
  do {
    char * serviceName = SERVICE_NAME;

    if (argc >= 2) {
      if (lstrcmpi(argv[1], "install"  ) == 0 ) {
        Q(ElevateIfNeeded());
        Q(SvcInstall(serviceName));
        break;
      } else if (lstrcmpi(argv[1], "uninstall") == 0 ) {
        Q(ElevateIfNeeded());
        Q(SvcUninstall(serviceName));
        break;
      } else if (lstrcmpi(argv[1], "run"      ) == 0 ) {
        printf("Running as console application\n");
        serviceMode = 0;
        SvcRun(argc, argv, serviceMode);
        break;
      }
    }

    SERVICE_TABLE_ENTRY DispatchTable[] = 
    { 
      { serviceName, (LPSERVICE_MAIN_FUNCTION) SvcMain },
      { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcher(DispatchTable)) { 
      TCHAR * exeName = strrchr(argv[0], '\\');
      if (exeName != NULL) {exeName++;} else {exeName = argv[0];}
      printf("Usage:\n");
      printf("\n%s install\n   Install service\n"  , exeName);
      printf("\n%s uninstall\n   Uninstall service\n", exeName);
      printf("\n%s run\n   Run in console mode\n"  , exeName);
    } else {
      serviceMode = 1;
    }
  } while (0);

  if (sc != ERROR_SUCCESS && !serviceMode) {
    PrintError(sc);
  }
  if (argc >= 3 && lstrcmpi(argv[2], "-pause") == 0 ) {
    printf("Press any key to continue...\n");
    getch();
  }
  return 0;
}

void wlog(char * line)
{
  TCHAR szPath[MAX_PATH+100];
  if(!GetModuleFileName( NULL, szPath, MAX_PATH ) ) { return; }
	lstrcpy(szPath+(lstrlen(szPath)-3), "log");
	FILE * fp = fopen(szPath, "a");
  time_t gt = time(0);
  fprintf(fp, "%s  %s\n", asctime(localtime(&gt)), line);
  fclose(fp);
}

void PrintError(LONG messageId)
{
  LPTSTR lpBuffer = NULL;
  int charsNum = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, messageId, 0, (LPTSTR)&lpBuffer, 1, NULL);
  if (charsNum > 0 && CharToOem(lpBuffer, lpBuffer)!=0) {
    printf("%i - %s\n", messageId, lpBuffer);
  } else {
    printf("Error code: %s\n", GetLastError());
  }
  LocalFree(lpBuffer);
}

LONG WaitForRegistryChanges()
{
	LONG sc = ERROR_SUCCESS;
  HKEY hkMonitorKey = INVALID_HANDLE_VALUE;
  HANDLE hMonitorEvent = INVALID_HANDLE_VALUE;
  BOOL bExit = FALSE;

  do {
    TCHAR subKey[] = REG_KEY;
    Q_LAST_IF(hMonitorEvent = CreateEvent(NULL, TRUE, FALSE, NULL), hMonitorEvent == NULL);
    HANDLE waitForHandles[2];
    waitForHandles[0] = ghStopEvent;
    waitForHandles[1] = hMonitorEvent;

    Q(RegOpenKeyEx(REG_ROOT, subKey, 0, KEY_NOTIFY | KEY_ENUMERATE_SUB_KEYS, &hkMonitorKey));
    Q(RegNotifyChangeKeyValue(hkMonitorKey, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, hMonitorEvent, TRUE));
    Q(LookupForChanges(hkMonitorKey, 0));
    
    while (!bExit) {
      DWORD waitResult = WaitForMultipleObjects(sizeof(waitForHandles)/sizeof(HANDLE), waitForHandles, FALSE, INFINITE);

      Q_LAST_IF(waitResult == WAIT_FAILED);
      switch (waitResult - WAIT_OBJECT_0) {
        case 0: // ghStopEvent
          bExit = TRUE;
          break;
        case 1: // hMonitorEvent
          Sleep(30);
          Q(LookupForChanges(hkMonitorKey, 0));
          break;
      } Q_IF_ERRSC();
    } Q_IF_ERRSC();

  } while (0);
  
  if (hkMonitorKey != INVALID_HANDLE_VALUE && hkMonitorKey != 0) {
    RegCloseKey(hkMonitorKey);
    hkMonitorKey = INVALID_HANDLE_VALUE;
  }
  CloseAndInvalidateHandle(&hMonitorEvent);
  return sc;
}

BOOL CloseAndInvalidateHandle(PHANDLE pHandle)
{
  if (*pHandle != INVALID_HANDLE_VALUE && *pHandle != 0) {
    CloseHandle(*pHandle);
    *pHandle = INVALID_HANDLE_VALUE;
  }
}

LONG LookupForChanges(HKEY hKey, int level)
{
  LONG sc = ERROR_SUCCESS;
  do {
    
    HKEY hkSubKey = INVALID_HANDLE_VALUE;
    DWORD dwIndex = 0;
    LONG enumResult = ERROR_SUCCESS;

    TCHAR keyName[MAX_KEY_LENGTH];
    DWORD maxKeyNameLength = MAX_KEY_LENGTH;
    DWORD value;
    DWORD valueLen = sizeof(value);

    while (enumResult = RegEnumKeyEx(hKey, dwIndex++, keyName, &maxKeyNameLength, NULL, NULL, NULL, NULL), enumResult == ERROR_SUCCESS) {
      maxKeyNameLength = MAX_KEY_LENGTH;
      CharToOem(keyName, keyName);

      sc = RegOpenKeyEx(hKey, keyName, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE, &hkSubKey);
      if (sc == ERROR_ACCESS_DENIED) { // skipping registry access errors
        sc = ERROR_SUCCESS;
        continue;
      } Q_IF_ERRSC();

      sc = RegGetValue(hkSubKey, NULL, REG_VALUE_NAME, RRF_RT_DWORD, NULL, (PVOID)&value, &valueLen);
      if (sc == ERROR_SUCCESS) {

        if (value != REG_NEW_VALUE) {
          value = REG_NEW_VALUE;
          Q(RegSetValueEx(hkSubKey, REG_VALUE_NAME, 0, REG_DWORD, (const BYTE *)&value, valueLen));
          wlog("Replaced");
        }

      } else if (sc == ERROR_FILE_NOT_FOUND) { // continue if no value with such name
        sc = ERROR_SUCCESS;
      }
      Q_IF_ERRSC();

      if (level<REG_MAX_LEVEL) {
        Q(LookupForChanges(hkSubKey, level+1));
      }
      Q(RegCloseKey(hkSubKey));
    }
    Q_IF_ERRSC();
    if (enumResult != ERROR_NO_MORE_ITEMS) {
      Q(enumResult);
    }
  } while (0);
  return sc;
}

LONG SvcInstall(char * svcname)
{
  LONG sc = ERROR_SUCCESS;
  SC_HANDLE schSCManager = NULL, schService = NULL;
  do {
    TCHAR szPath[MAX_PATH];
    
    printf("Installing service '%s'\n", svcname);
    Q_LAST_IF(!GetModuleFileName(NULL, szPath, MAX_PATH));
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    Q_LAST_IF(NULL == schSCManager);
    schService = CreateService(schSCManager, svcname, SERVICE_DISPLAY_NAME, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                               SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, szPath, NULL, NULL, NULL, NULL, NULL);
    Q_LAST_IF(schService == NULL);
    printf("Service installed successfully\n");
    
    Q_LAST_IF(!StartService(schService, 0, NULL));
    printf("Service started\n");

  } while (0);
  if (schService != NULL) { CloseServiceHandle(schService); }
  if (schSCManager != NULL) { CloseServiceHandle(schSCManager); }
  return sc;
}

LONG SvcUninstall(char * svcname)
{
  LONG sc = ERROR_SUCCESS;
  SC_HANDLE schSCManager = NULL, schService = NULL;
  SERVICE_STATUS_PROCESS ssp;
  do {
    TCHAR szPath[MAX_PATH];

    printf("Uninstalling service '%s'\n", svcname);
    Q_LAST_IF(!GetModuleFileName(NULL, szPath, MAX_PATH));
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    Q_LAST_IF(NULL == schSCManager);
    schService = OpenService(schSCManager, svcname, DELETE | SERVICE_STOP);
    Q_LAST_IF(schService == NULL);
    printf("Stopping service\n");
    if (!ControlService(schService, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&ssp)) {
      PrintError(GetLastError());
    }
    Q_LAST_IF(!DeleteService(schService))
    schService = NULL;
		printf("Service uninstalled successfully\n");
  } while (0);
  if (schService != NULL) { CloseServiceHandle(schService); }
  if (schSCManager != NULL) { CloseServiceHandle(schSCManager); }
  return sc;
}

VOID WINAPI SvcMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
  gSvcStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, SvcCtrlHandler);
  if(!gSvcStatusHandle) { 
    return; 
  } 
  gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  gSvcStatus.dwServiceSpecificExitCode = 0;
  ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000 );

  SvcRun(dwArgc, lpszArgv, TRUE);
}

VOID WINAPI SvcCtrlHandler(DWORD dwCtrl)
{
  if (dwCtrl == SERVICE_CONTROL_STOP || dwCtrl == SERVICE_CONTROL_SHUTDOWN) {
    ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
    SetEvent(ghStopEvent);
    ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);
  } else if (dwCtrl == SERVICE_CONTROL_INTERROGATE) {
    ReportSvcStatus( SERVICE_RUNNING, NO_ERROR, 0 );
  }
}

VOID ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
    static DWORD dwCheckPoint = 1;

    gSvcStatus.dwCurrentState = dwCurrentState;
    gSvcStatus.dwWin32ExitCode = dwWin32ExitCode;
    gSvcStatus.dwWaitHint = dwWaitHint;
    
    if (dwCurrentState == SERVICE_START_PENDING) {
      gSvcStatus.dwControlsAccepted = 0;
    } else {
      gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    }
    
    if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED)) {
      gSvcStatus.dwCheckPoint = 0;
    } else {
      gSvcStatus.dwCheckPoint = dwCheckPoint++;
    }
    SetServiceStatus(gSvcStatusHandle, &gSvcStatus);
}


VOID SvcRun(DWORD dwArgc, LPTSTR *lpszArgv, BOOL runAsService)
{
  LONG sc = ERROR_SUCCESS;
  DWORD retryCount = 0;
  do {
    Q_LAST_IF(!SetConsoleCtrlHandler(ConsoleCtrlHandlerRoutine, TRUE));
    Q_LAST_IF(ghStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL), ghStopEvent == NULL);
    
    ReportSvcStatus( SERVICE_RUNNING, NO_ERROR, 0 );

    for (;;) {
      int errorCode = WaitForRegistryChanges();
      if (errorCode == ERROR_SUCCESS) {
        break;
      }
      if (++retryCount%2000) { // something went wrong too many times
        Sleep(600000); // sleep 10 min
      }
    }
  } while (0);
  CloseAndInvalidateHandle(&ghStopEvent);
  ReportSvcStatus( SERVICE_STOPPED, NO_ERROR, 0 );

  if (!runAsService && sc != ERROR_SUCCESS) {
    PrintError(sc);
  }
}

LONG ElevateIfNeeded()
{
  LONG sc = ERROR_SUCCESS;
  PSID pAdministratorsGroup = NULL;
  do {
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    
    Q_LAST_IF(!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                        0, 0, 0, 0, 0, 0, &pAdministratorsGroup));
    BOOL fIsRunAsAdmin = FALSE;
    Q_LAST_IF(!CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin));
    
    if (!fIsRunAsAdmin) { // Elevate
      printf("Starting as administrator...\n");
      TCHAR szPath[MAX_PATH];
      Q_LAST_IF(!GetModuleFileName(NULL, szPath, MAX_PATH));
      
      TCHAR szParams[256] = "";
      for (int i=1; i<__argc; i++) {
        if (i>1) { strncat(szParams, " ", sizeof(szParams)-(strlen(szParams)+1)); }
        strncat(szParams, __argv[i], sizeof(szParams)-(strlen(szParams)+1));
      }
      strncat(szParams, " -pause", sizeof(szParams)-(strlen(szParams)+1));

      SHELLEXECUTEINFO sei = { sizeof(sei) };
      sei.lpVerb = "runas";
      sei.lpFile = szPath;
      sei.lpParameters = szParams;
      sei.nShow = SW_NORMAL;
      Q_LAST_IF(!ShellExecuteEx(&sei));
      exit(0);
    }
  } while (0);
  if (pAdministratorsGroup != NULL) { FreeSid(pAdministratorsGroup); }
  return sc;
}