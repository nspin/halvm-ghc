#include "Rts.h"
#include "Prelude.h"
#include <assert.h>
#include "iomanager.h"
#include "time_rts.h"
#include "Task.h"
#include "Schedule.h"
#include "smp.h"

void setIOManagerControlFd(int fd)
{
  printf("ERROR: Someone called setIOManagerControlFd(%d)\n", fd);
  assert(0);
}

void setIOManagerWakeupFd(int fd)
{
  printf("ERROR: Someone called setIOManagerWakeupFd(%d)\n", fd);
  assert(0);
}

void ioManagerWakeup(void)
{
  /* nothin'! */
}

#ifdef THREADED_RTS
typedef struct waiter {
  struct waiter *next;
  StgWord target;
  StgStablePtr action;
} waiter_t;

static halvm_mutex_t  waiters_lock;
static waiter_t      *waiters = NULL;
#endif

void registerWaiter(int usecs MUNUSED, StgStablePtr action MUNUSED)
{
#ifdef THREADED_RTS
  waiter_t *newWaiter = malloc(sizeof(waiter_t));
  waiter_t *cur, *prev;

  newWaiter->target = getDelayTarget(usecs);
  newWaiter->action = action;

  halvm_acquire_lock(&waiters_lock);
  for(cur = waiters, prev = NULL; cur; prev = cur, cur = cur->next)
    if(cur->target > newWaiter->target) {
      newWaiter->next = cur;
      if(prev) prev->next = newWaiter; else waiters = newWaiter;
      halvm_release_lock(&waiters_lock);
      return;
    }

  newWaiter->next = NULL;
  if(prev) prev->next = newWaiter; else waiters = newWaiter;
  halvm_release_lock(&waiters_lock);
  pokeSleepThread();
#endif
}

StgStablePtr waitForWaiter()
{
#ifdef THREADED_RTS
  while(1) {
    StgStablePtr signal = dequeueSignalHandler();
    unsigned long target;

    if(signal) {
      return signal;
    }

    halvm_acquire_lock(&waiters_lock);
    if(waiters && waiters->target <= getDelayTarget(0)) {
      StgStablePtr retval = waiters->action;
      waiter_t *dead = waiters;

      waiters = waiters->next;
      halvm_release_lock(&waiters_lock);
      free(dead);
      return retval;
    }
    target = waiters ? waiters->target : getDelayTarget(6000000);
    halvm_release_lock(&waiters_lock);

    sleepUntilWaiter(target);
  }
#else
  assert(0);
  return NULL;
#endif
}

#ifdef THREADED_RTS
void ioManagerDie(void)
{
  if(waiters) {
    printf("WARNING: IO Manager is dying with people waiting to run.\n");
  }
}

void ioManagerStart(void)
{
  Capability *cap;

  initMutex(&waiters_lock);
  cap = rts_lock();
  rts_evalIO(&cap, &base_GHCziConcziIO_ensureIOManagerIsRunning_closure, NULL);
  rts_unlock(cap);
}
#endif