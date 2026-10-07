#pragma once

namespace rb {

// Runs the HTTP server under the Service Control Manager when the
// process was started by the SCM.
//
// Returns true when the SCM owned the process (the caller should exit),
// false when running as an ordinary console process (the caller should
// start the server normally).
bool tryRunAsService();

}  // namespace rb
