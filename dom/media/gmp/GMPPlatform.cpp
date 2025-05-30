/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GMPPlatform.h"
#include "GMPStorageChild.h"
#include "GMPTimerChild.h"
#include "mozilla/Monitor.h"
#include "GMPChild.h"
#include "mozilla/Mutex.h"
#include "mozilla/ReentrantMonitor.h"
#include "mozilla/StaticMonitor.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"
#include "base/task.h"
#include "base/thread.h"
#include "base/time.h"

#ifdef XP_WIN
#  include "mozilla/UntrustedModulesProcessor.h"
#endif

#include <ctime>

namespace mozilla::gmp {

static MessageLoop* sMainLoop = nullptr;
static GMPChild* sChild = nullptr;

static StaticMonitor sMainLoopMonitor;
static nsTArray<RefPtr<Runnable>>* sMainLoopPendingEvents
    MOZ_GUARDED_BY(sMainLoopMonitor) = nullptr;
static bool sMainLoopHasPendingProcess MOZ_GUARDED_BY(sMainLoopMonitor) = false;

static bool IsOnChildMainThread() {
  return sMainLoop && sMainLoop == MessageLoop::current();
}

// We just need a refcounted wrapper for GMPTask objects.
class GMPRunnable final : public Runnable {
 public:
  explicit GMPRunnable(GMPTask* aTask)
      : Runnable("mozilla::gmp::GMPRunnable"), mTask(aTask) {
    MOZ_ASSERT(mTask);
  }

  NS_IMETHOD Run() override {
    mTask->Run();
    mTask->Destroy();
    mTask = nullptr;
    return NS_OK;
  }

 private:
  GMPTask* mTask;
};

class GMPSyncRunnable final : public Runnable {
 public:
  GMPSyncRunnable(GMPTask* aTask, MessageLoop* aMessageLoop)
      : Runnable("mozilla::gmp::GMPSyncRunnable"),
        mDone(false),
        mTask(aTask),
        mMonitor("GMPSyncRunnable") {
    MOZ_ASSERT(mTask);
  }

  void WaitUntilDone() {
    // We assert here for two reasons.
    // 1) Nobody should be blocking the main thread.
    // 2) This prevents deadlocks when doing sync calls to main which if the
    //    main thread tries to do a sync call back to the calling thread.
    MOZ_ASSERT(!IsOnChildMainThread());

    MonitorAutoLock lock(mMonitor);
    while (!mDone) {
      lock.Wait();
    }
  }

  NS_IMETHOD Run() override {
    mTask->Run();
    mTask->Destroy();
    mTask = nullptr;
    MonitorAutoLock lock(mMonitor);
    mDone = true;
    lock.Notify();
    return NS_OK;
  }

 private:
  bool mDone MOZ_GUARDED_BY(mMonitor);
  GMPTask* mTask;
  Monitor mMonitor;
};

class GMPThreadImpl final : public GMPThread {
 public:
  GMPThreadImpl();
  virtual ~GMPThreadImpl();

  // GMPThread
  void Post(GMPTask* aTask) override;
  void Join() override;

 private:
  Mutex mMutex MOZ_UNANNOTATED;
  base::Thread mThread MOZ_GUARDED_BY(mMutex);
};

GMPErr CreateThread(GMPThread** aThread) {
  if (!aThread) {
    return GMPGenericErr;
  }

  *aThread = new GMPThreadImpl();

  return GMPNoErr;
}

bool SpinPendingGmpEventsUntil(const SpinPendingPredicate& aPred,
                               uint32_t aTimeoutMs) {
  MOZ_ASSERT(IsOnChildMainThread());

  auto timeout = TimeDuration::FromMilliseconds(aTimeoutMs);

  while (!aPred()) {
    nsTArray<RefPtr<Runnable>> pendingEvents;
    {
      StaticMonitorAutoLock lock(sMainLoopMonitor);
      while (sMainLoopPendingEvents->IsEmpty()) {
        if (lock.Wait(timeout) == CVStatus::Timeout) {
          return false;
        }
      }
      pendingEvents = std::move(*sMainLoopPendingEvents);
    }

    for (auto& event : pendingEvents) {
      event->Run();
    }
  }

  return true;
}

static void ProcessPendingGmpEvents() {
  MOZ_ASSERT(IsOnChildMainThread());

  nsTArray<RefPtr<Runnable>> pendingEvents;
  {
    StaticMonitorAutoLock lock(sMainLoopMonitor);
    pendingEvents = std::move(*sMainLoopPendingEvents);
    sMainLoopHasPendingProcess = false;
  }

  for (auto& event : pendingEvents) {
    event->Run();
  }
}

static void QueueForMainThread(RefPtr<Runnable>&& aRunnable) {
  StaticMonitorAutoLock lock(sMainLoopMonitor);
  sMainLoopPendingEvents->AppendElement(std::move(aRunnable));
  if (!sMainLoopHasPendingProcess) {
    sMainLoop->PostTask(NewRunnableFunction(
        "mozilla::gmp::ProcessPendingGmpEvents", &ProcessPendingGmpEvents));
    sMainLoopHasPendingProcess = true;
  }
  lock.Notify();
}

GMPErr RunOnMainThread(GMPTask* aTask) {
  if (!aTask || !sMainLoop) {
    return GMPGenericErr;
  }

  RefPtr<GMPRunnable> r = new GMPRunnable(aTask);
  QueueForMainThread(std::move(r));
  return GMPNoErr;
}

GMPErr SyncRunOnMainThread(GMPTask* aTask) {
  if (!aTask || !sMainLoop || IsOnChildMainThread()) {
    return GMPGenericErr;
  }

  RefPtr<GMPSyncRunnable> r = new GMPSyncRunnable(aTask, sMainLoop);
  QueueForMainThread(RefPtr{r});
  r->WaitUntilDone();

  return GMPNoErr;
}

class MOZ_CAPABILITY("mutex") GMPMutexImpl final : public GMPMutex {
 public:
  GMPMutexImpl();
  virtual ~GMPMutexImpl();

  // GMPMutex
  void Acquire() override MOZ_CAPABILITY_ACQUIRE();
  void Release() override MOZ_CAPABILITY_RELEASE();
  void Destroy() override;

 private:
  ReentrantMonitor mMonitor MOZ_UNANNOTATED;
};

GMPErr CreateMutex(GMPMutex** aMutex) {
  if (!aMutex) {
    return GMPGenericErr;
  }

  *aMutex = new GMPMutexImpl();

  return GMPNoErr;
}

GMPErr CreateRecord(const char* aRecordName, uint32_t aRecordNameSize,
                    GMPRecord** aOutRecord, GMPRecordClient* aClient) {
  if (aRecordNameSize > GMP_MAX_RECORD_NAME_SIZE || aRecordNameSize == 0) {
    NS_WARNING("GMP tried to CreateRecord with too long or 0 record name");
    return GMPGenericErr;
  }
  GMPStorageChild* storage = sChild->GetGMPStorage();
  if (!storage) {
    return GMPGenericErr;
  }
  MOZ_ASSERT(storage);
  return storage->CreateRecord(nsDependentCString(aRecordName, aRecordNameSize),
                               aOutRecord, aClient);
}

GMPErr SetTimerOnMainThread(GMPTask* aTask, int64_t aTimeoutMS) {
  if (!aTask || !sMainLoop || !IsOnChildMainThread()) {
    return GMPGenericErr;
  }
  GMPTimerChild* timers = sChild->GetGMPTimers();
  NS_ENSURE_TRUE(timers, GMPGenericErr);
  return timers->SetTimer(aTask, aTimeoutMS);
}

GMPErr GetClock(GMPTimestamp* aOutTime) {
  if (!aOutTime) {
    return GMPGenericErr;
  }
  *aOutTime = base::Time::Now().ToDoubleT() * 1000.0;
  return GMPNoErr;
}

void InitPlatformAPI(GMPPlatformAPI& aPlatformAPI, GMPChild* aChild) {
  if (!sMainLoop) {
    sMainLoop = MessageLoop::current();
  }
  if (!sChild) {
    sChild = aChild;
  }

  {
    StaticMonitorAutoLock lock(sMainLoopMonitor);
    if (!sMainLoopPendingEvents) {
      sMainLoopPendingEvents = new nsTArray<RefPtr<Runnable>>();
    }
  }

  aPlatformAPI.version = 0;
  aPlatformAPI.createthread = &CreateThread;
  aPlatformAPI.runonmainthread = &RunOnMainThread;
  aPlatformAPI.syncrunonmainthread = &SyncRunOnMainThread;
  aPlatformAPI.createmutex = &CreateMutex;
  aPlatformAPI.createrecord = &CreateRecord;
  aPlatformAPI.settimer = &SetTimerOnMainThread;
  aPlatformAPI.getcurrenttime = &GetClock;
}

void ShutdownPlatformAPI() {
  StaticMonitorAutoLock lock(sMainLoopMonitor);
  if (sMainLoopPendingEvents) {
    delete sMainLoopPendingEvents;
    sMainLoopPendingEvents = nullptr;
  }
}

void SendFOGData(ipc::ByteBuf&& buf) {
  if (sChild) {
    sChild->SendFOGData(std::move(buf));
  }
}

#ifdef XP_WIN
RefPtr<PGMPChild::GetModulesTrustPromise> SendGetModulesTrust(
    ModulePaths&& aModules, bool aRunAtNormalPriority) {
  if (!sChild) {
    return PGMPChild::GetModulesTrustPromise::CreateAndReject(
        ipc::ResponseRejectReason::SendError, __func__);
  }
  return sChild->SendGetModulesTrust(std::move(aModules), aRunAtNormalPriority);
}
#endif

GMPThreadImpl::GMPThreadImpl() : mMutex("GMPThreadImpl"), mThread("GMPThread") {
  MOZ_COUNT_CTOR(GMPThread);
}

GMPThreadImpl::~GMPThreadImpl() { MOZ_COUNT_DTOR(GMPThread); }

void GMPThreadImpl::Post(GMPTask* aTask) {
  MutexAutoLock lock(mMutex);

  if (!mThread.IsRunning()) {
    bool started = mThread.Start();
    if (!started) {
      NS_WARNING("Unable to start GMPThread!");
      return;
    }
  }

  RefPtr<GMPRunnable> r = new GMPRunnable(aTask);
  mThread.message_loop()->PostTask(
      NewRunnableMethod("gmp::GMPRunnable::Run", r.get(), &GMPRunnable::Run));
}

void GMPThreadImpl::Join() {
  {
    MutexAutoLock lock(mMutex);
    if (mThread.IsRunning()) {
      mThread.Stop();
    }
  }
  delete this;
}

GMPMutexImpl::GMPMutexImpl() : mMonitor("gmp-mutex") {
  MOZ_COUNT_CTOR(GMPMutexImpl);
}

GMPMutexImpl::~GMPMutexImpl() { MOZ_COUNT_DTOR(GMPMutexImpl); }

void GMPMutexImpl::Destroy() { delete this; }

MOZ_PUSH_IGNORE_THREAD_SAFETY
void GMPMutexImpl::Acquire() { mMonitor.Enter(); }

void GMPMutexImpl::Release() { mMonitor.Exit(); }
MOZ_POP_THREAD_SAFETY

GMPTask* NewGMPTask(std::function<void()>&& aFunction) {
  class Task : public GMPTask {
   public:
    explicit Task(std::function<void()>&& aFunction)
        : mFunction(std::move(aFunction)) {}
    void Destroy() override { delete this; }
    ~Task() override = default;
    void Run() override { mFunction(); }

   private:
    std::function<void()> mFunction;
  };
  return new Task(std::move(aFunction));
}

}  // namespace mozilla::gmp
