#include "service.hpp"

#include <windows.h>

#include "http.hpp"

namespace rb {
namespace {

// Must match the name the installer registers with the SCM.
constexpr const char* kServiceName = "RemoteBuilderAgent";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};

void WINAPI ctrlHandler(DWORD ctrl) {
  switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      g_status.dwCurrentState = SERVICE_STOP_PENDING;
      SetServiceStatus(g_statusHandle, &g_status);
      // The HTTP server runs until the process ends, so stopping the
      // service simply ends the process.
      ExitProcess(0);
    case SERVICE_CONTROL_INTERROGATE:
      SetServiceStatus(g_statusHandle, &g_status);
      break;
    default:
      break;
  }
}

void WINAPI serviceMain(DWORD, LPSTR*) {
  g_statusHandle = RegisterServiceCtrlHandlerA(kServiceName, ctrlHandler);
  if (g_statusHandle == nullptr) return;

  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  g_status.dwCurrentState = SERVICE_RUNNING;
  g_status.dwWin32ExitCode = NO_ERROR;
  SetServiceStatus(g_statusHandle, &g_status);

  runServer();

  g_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(g_statusHandle, &g_status);
}

}  // namespace

bool tryRunAsService() {
  SERVICE_TABLE_ENTRYA table[] = {
      {const_cast<LPSTR>(kServiceName), serviceMain},
      {nullptr, nullptr},
  };
  if (StartServiceCtrlDispatcherA(table)) return true;
  // A console launch reports ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
  // anything else means we were started as a service but failed to
  // connect, so do not fall back to a console run.
  return GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
}

}  // namespace rb
