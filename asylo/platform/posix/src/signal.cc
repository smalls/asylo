/*
 *
 * Copyright 2017 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <signal.h>

#include <pthread.h>
#include <cstdlib>

#include "absl/synchronization/mutex.h"
#include "asylo/platform/arch/include/trusted/host_calls.h"
#include "asylo/platform/arch/include/trusted/register_signal.h"
#include "asylo/platform/core/trusted_global_state.h"
#include "asylo/platform/posix/signal/signal_manager.h"

extern "C" {

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) { abort(); }

// Registers a signal handler for |signum|.
//
// This method registers a signal handler in two parts:
// * On the host-side, registers a signal-handling function that calls into the
// enclave to handle the signal.
// * Inside the enclave, registers the user-provided function |act| as the
// actual signal handler.
//
// When a signal is subsequently invoked on the host, it will first be passed to
// the host-side signal handler. The host-side signal handler will then enter
// enclave to invoke the corresponding signal handler registered inside the
// enclave.
// Note that the behavior described above applies if either the enclave is run
// in hardware mode, or it is run in simulation mode and the TCS is inactive. If
// the enclave is run in simulation mode and TCS is active (i.e. a thread is
// running inside the enclave), then this function will call the signal handler
// registered inside the enclave directly.
int sigaction(int signum, const struct sigaction *act,
              struct sigaction *oldact) {
  // Guards sigaction calls. This is to ensure that signal handlers are not
  // overwritten between the time sigaction gets |oldact| and sets |act|.
  static absl::Mutex sigaction_lock;
  {
    absl::MutexLock lock(&sigaction_lock);
    asylo::SignalManager *signal_manager = asylo::SignalManager::GetInstance();
    if (oldact) {
      if (signal_manager->GetSigAction(signum)) {
        oldact = const_cast<struct sigaction *>(
            signal_manager->GetSigAction(signum));
      } else {
        oldact->sa_handler = SIG_DFL;
      }
    }
    signal_manager->SetSigAction(signum, *act);
  }
  sigset_t mask;
  sigemptyset(&mask);
  if (act) {
    mask = act->sa_mask;
  }
  const std::string enclave_name = asylo::GetEnclaveName();
  // Pass a C string because enc_register_signal has C linkage. This string is
  // copied to untrusted memory when going across enclave boundary.
  return enc_register_signal(signum, mask, enclave_name.c_str());
}

// Sets the signal mask with |set|.
//
// This method sets the signal mask both inside the enclave and on the host.
// If |how| is SIG_UNBLOCK, the signals to unblock are unblocked inside the
// enclave first, then on the host.
// If |how| is SIG_BLOCK, the signals to block are blocked on the host first,
// then inside the enclave.
// If |how| is SIG_SETMASK, |set| is separated to signals to block and to
// unblock. The signals to unblock are processed inside the enclave first, then
// on the host, and the signals to block are processed on the host first, then
// inside the enclave.
// |oldset| is set to the signal mask used inside the enclave prior to this
// call.
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
    errno = EINVAL;
    return -1;
  }
  asylo::SignalManager *signal_manager = asylo::SignalManager::GetInstance();
  if (oldset) {
    *oldset = signal_manager->GetSignalMask();
  }
  if (!set) {
    return 0;
  }
  sigset_t signals_to_block;
  sigemptyset(&signals_to_block);
  sigset_t signals_to_unblock;
  sigemptyset(&signals_to_unblock);
  if (how == SIG_BLOCK || how == SIG_SETMASK) {
    signals_to_block = *set;
  }

  if (how == SIG_UNBLOCK) {
    signals_to_unblock = *set;
  } else if (how == SIG_SETMASK) {
    signals_to_unblock = signal_manager->GetUnblockedSet(*set);
  }

  // Unblock signals inside the enclave before unblocking signals on the host.
  signal_manager->UnblockSignals(signals_to_unblock);

  // |oldset| is already filled with the signal mask inside the enclave.
  int res = enc_untrusted_sigprocmask(how, set, /*oldset=*/nullptr);

  // Block signals inside the enclave after the host.
  signal_manager->BlockSignals(signals_to_block);

  return res;
}

// Registers a signal handler for |signum| with |handler|.
//
// This method is a special case of sigaction. It calls sigaction with only
// sa_handler in |act| field set.
sighandler_t signal(int signum, sighandler_t handler) {
  struct sigaction act;
  act.sa_handler = handler;
  struct sigaction oldact;
  if (sigaction(signum, &act, &oldact)) {
    // Errno is set by sigaction.
    return SIG_ERR;
  }
  return oldact.sa_handler;
}

}  // extern "C"
