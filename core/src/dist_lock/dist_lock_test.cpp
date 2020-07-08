#include <utest/utest.hpp>

#include <concurrent/variable.hpp>
#include <dist_lock/dist_lock_settings.hpp>
#include <dist_lock/dist_lock_strategy.hpp>
#include <dist_lock/dist_locked_task.hpp>
#include <dist_lock/dist_locked_worker.hpp>
#include <engine/condition_variable.hpp>
#include <engine/mutex.hpp>
#include <engine/sleep.hpp>
#include <engine/task/cancel.hpp>
#include <logging/log.hpp>
#include <utils/async.hpp>
#include <utils/datetime.hpp>
#include <utils/mock_now.hpp>

namespace {

constexpr std::chrono::milliseconds kAttemptInterval{10};
constexpr std::chrono::milliseconds kLockTtl{100};
constexpr auto kAttemptTimeout = 5 * kAttemptInterval;

const std::string kWorkerName = "test";

dist_lock::DistLockSettings MakeSettings() {
  return {kAttemptInterval, kAttemptInterval, kLockTtl, kAttemptInterval,
          kAttemptInterval};
}

class MockDistLockStrategy : public dist_lock::DistLockStrategyBase {
 public:
  ~MockDistLockStrategy() { EXPECT_FALSE(IsLocked()); }

  void Acquire(std::chrono::milliseconds,
               const std::string& locker_id) override {
    UASSERT(!locker_id.empty());
    attempts_++;
    auto locked_by = locked_by_var_.Lock();
    if (!locked_by->empty() && *locked_by != locker_id)
      throw dist_lock::LockIsAcquiredByAnotherHostException();
    if (!allowed_) throw std::runtime_error("not allowed");
    *locked_by = locker_id;
  }

  void Release(const std::string& locker_id) override {
    auto locked_by = locked_by_var_.Lock();
    if (*locked_by == locker_id) locked_by->clear();
  }

  bool IsLocked() {
    auto locked_by = locked_by_var_.Lock();
    return !locked_by->empty();
  }

  void Allow(bool allowed) { allowed_ = allowed; }

  void SetLockedBy(const std::string& whom) {
    auto locked_by = locked_by_var_.Lock();
    *locked_by = whom;
  }

  size_t GetAttemptsCount() const { return attempts_; }

 private:
  concurrent::Variable<std::string> locked_by_var_;
  std::atomic<bool> allowed_{false};
  std::atomic<size_t> attempts_{0};
};

auto MakeMockStrategy() { return std::make_shared<MockDistLockStrategy>(); }

class DistLockWorkload {
 public:
  explicit DistLockWorkload(bool abort_on_cancel = false)
      : abort_on_cancel_(abort_on_cancel) {}

  bool IsLocked() const { return is_locked_; }

  bool WaitForLocked(bool locked, std::chrono::milliseconds ms) {
    std::unique_lock<engine::Mutex> lock(mutex_);
    return cv_.WaitFor(lock, ms,
                       [locked, this] { return locked == IsLocked(); });
  }

  void SetWorkLoopOn(bool val) { work_loop_on_.store(val); }
  size_t GetStartedWorkCount() const { return work_start_count_.load(); }
  size_t GetFinishedWorkCount() const { return work_finish_count_.load(); }

  void Work() {
    LOG_DEBUG() << "work begin";
    SetLocked(true);
    work_start_count_++;
    LOG_DEBUG() << "work begin after SetLocked(true)";

    try {
      bool work_loop_on;
      while ((work_loop_on = work_loop_on_) &&
             !engine::current_task::IsCancelRequested()) {
        LOG_DEBUG() << "work loop";
        engine::InterruptibleSleepFor(std::chrono::milliseconds(50));
      }

      if (work_loop_on && abort_on_cancel_)
        engine::current_task::CancellationPoint();
    } catch (...) {
      SetLocked(false);
      throw;
    }

    LOG_DEBUG() << "work end";
    work_finish_count_++;
    SetLocked(false);
    LOG_DEBUG() << "work end after SetLocked(false)";
  }

 private:
  void SetLocked(bool locked) {
    std::unique_lock<engine::Mutex> lock(mutex_);
    is_locked_ = locked;
    cv_.NotifyAll();
  }

  const bool abort_on_cancel_;
  std::atomic<bool> is_locked_{false};
  std::atomic<bool> work_loop_on_{true};
  engine::Mutex mutex_;
  engine::ConditionVariable cv_;
  std::atomic<size_t> work_start_count_{0};
  std::atomic<size_t> work_finish_count_{0};
};

}  // namespace

TEST(LockedWorker, Noop) {
  RunInCoro([] {
    dist_lock::DistLockedWorker locked_worker(
        kWorkerName, [] {}, MakeMockStrategy(), MakeSettings());
  });
}

TEST(LockedWorker, StartStop) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;
        dist_lock::DistLockedWorker locked_worker(
            kWorkerName, [&] { work.Work(); }, strategy, MakeSettings());
        EXPECT_FALSE(work.IsLocked());

        locked_worker.Start();
        EXPECT_FALSE(work.WaitForLocked(true, kAttemptTimeout));

        strategy->Allow(true);
        EXPECT_TRUE(work.WaitForLocked(true, kMaxTestWaitTime));

        locked_worker.Stop();
      },
      3);
}

TEST(LockedWorker, Watchdog) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;
        dist_lock::DistLockedWorker locked_worker(
            kWorkerName, [&] { work.Work(); }, strategy, MakeSettings());

        locked_worker.Start();
        strategy->Allow(true);
        EXPECT_TRUE(work.WaitForLocked(true, kMaxTestWaitTime));

        strategy->Allow(false);
        EXPECT_TRUE(work.WaitForLocked(false, kMaxTestWaitTime));

        locked_worker.Stop();
      },
      3);
}

TEST(LockedWorker, OkAfterFail) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;
        dist_lock::DistLockedWorker locked_worker(
            kWorkerName, [&] { work.Work(); }, strategy, MakeSettings());

        locked_worker.Start();
        EXPECT_FALSE(work.WaitForLocked(true, kAttemptTimeout));
        auto fail_count = strategy->GetAttemptsCount();
        EXPECT_LT(0, fail_count);
        EXPECT_FALSE(work.IsLocked());

        strategy->Allow(true);
        EXPECT_TRUE(work.WaitForLocked(true, kMaxTestWaitTime));
        EXPECT_LT(fail_count, strategy->GetAttemptsCount());

        locked_worker.Stop();
      },
      3);
}

// TODO: TAXICOMMON-1059
TEST(LockedWorker, DISABLED_IN_MAC_OS_TEST_NAME(OkFailOk)) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;
        dist_lock::DistLockedWorker locked_worker(
            kWorkerName, [&] { work.Work(); }, strategy, MakeSettings());

        locked_worker.Start();
        strategy->Allow(true);
        EXPECT_TRUE(work.WaitForLocked(true, kMaxTestWaitTime));

        strategy->Allow(false);
        auto attempts_count = strategy->GetAttemptsCount();
        EXPECT_LT(0, attempts_count);
        EXPECT_FALSE(work.WaitForLocked(false, kAttemptTimeout));

        auto attempts_count2 = strategy->GetAttemptsCount();
        EXPECT_LT(attempts_count, attempts_count2);

        strategy->Allow(true);
        // FIXME
        EXPECT_FALSE(work.WaitForLocked(false, kAttemptTimeout));
        auto attempts_count3 = strategy->GetAttemptsCount();
        EXPECT_LT(attempts_count2, attempts_count3);

        locked_worker.Stop();
      },
      3);
}

TEST(LockedWorker, LockedByOther) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;
        dist_lock::DistLockedWorker locked_worker(
            kWorkerName, [&] { work.Work(); }, strategy, MakeSettings());

        locked_worker.Start();
        strategy->Allow(true);
        EXPECT_TRUE(work.WaitForLocked(true, kMaxTestWaitTime));

        strategy->SetLockedBy("me");
        EXPECT_TRUE(work.WaitForLocked(false, kMaxTestWaitTime));

        strategy->Release("me");
        EXPECT_TRUE(work.WaitForLocked(false, kMaxTestWaitTime));

        locked_worker.Stop();
      },
      3);
}

TEST(LockedTask, Smoke) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;
        dist_lock::DistLockedTask locked_task(kWorkerName, [&] { work.Work(); },
                                              strategy, MakeSettings());

        EXPECT_EQ(0u, work.GetFinishedWorkCount());
        strategy->Allow(true);
        EXPECT_TRUE(work.WaitForLocked(true, kAttemptTimeout));

        work.SetWorkLoopOn(false);
        strategy->Allow(false);
        locked_task.WaitFor(kMaxTestWaitTime);
        EXPECT_TRUE(locked_task.GetState() == engine::Task::State::kCompleted);
        EXPECT_EQ(1u, work.GetFinishedWorkCount());
      },
      3);
}

TEST(LockedTask, SingleAttempt) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;
        std::atomic<size_t> counter{0};
        dist_lock::DistLockedTask locked_task(
            kWorkerName,
            [&] {
              counter++;
              throw std::runtime_error("123");
            },
            strategy, MakeSettings(), dist_lock::DistLockWaitingMode::kWait,
            dist_lock::DistLockRetryMode::kSingleAttempt);

        EXPECT_EQ(0u, work.GetFinishedWorkCount());
        strategy->Allow(true);

        locked_task.WaitFor(kMaxTestWaitTime);
        ASSERT_TRUE(locked_task.IsFinished());
        try {
          locked_task.Get();
          FAIL() << "Should have thrown";
        } catch (const std::runtime_error& e) {
          EXPECT_EQ(e.what(), std::string{"123"});
        }
        EXPECT_EQ(counter.load(), 1);

        EXPECT_EQ(0u, work.GetFinishedWorkCount());
      },
      3);
}

TEST(LockedTask, Fail) {
  RunInCoro(
      [] {
        auto settings = MakeSettings();
        settings.prolong_interval += settings.lock_ttl;  // make watchdog fire

        auto strategy = MakeMockStrategy();
        DistLockWorkload work(true);
        dist_lock::DistLockedTask locked_task(kWorkerName, [&] { work.Work(); },
                                              strategy, settings);

        EXPECT_EQ(0u, work.GetStartedWorkCount());
        EXPECT_EQ(0u, work.GetFinishedWorkCount());
        strategy->Allow(true);

        EXPECT_TRUE(work.WaitForLocked(true, kAttemptTimeout));
        locked_task.WaitFor(settings.prolong_interval + kAttemptTimeout);
        EXPECT_FALSE(locked_task.IsFinished());
        EXPECT_TRUE(work.WaitForLocked(true, kAttemptTimeout));
        EXPECT_TRUE(work.WaitForLocked(false, kMaxTestWaitTime));

        EXPECT_LE(1u, work.GetStartedWorkCount());
        EXPECT_EQ(0u, work.GetFinishedWorkCount());
      },
      3);
}

TEST(LockedTask, NoWait) {
  RunInCoro(
      [] {
        auto settings = MakeSettings();

        auto strategy = MakeMockStrategy();
        strategy->SetLockedBy("me");

        DistLockWorkload work(true);
        dist_lock::DistLockedTask locked_task(
            kWorkerName, [&] { work.Work(); }, strategy, settings,
            dist_lock::DistLockWaitingMode::kNoWait);

        engine::InterruptibleSleepFor(3 * settings.prolong_interval);

        EXPECT_EQ(1, strategy->GetAttemptsCount());

        EXPECT_TRUE(locked_task.IsFinished());
        EXPECT_EQ(0u, work.GetStartedWorkCount());
        EXPECT_EQ(0u, work.GetFinishedWorkCount());
        strategy->Release("me");
      },
      3);
}

TEST(LockedTask, NoWaitAquire) {
  RunInCoro(
      [] {
        auto strategy = MakeMockStrategy();
        DistLockWorkload work;

        EXPECT_EQ(0u, work.GetFinishedWorkCount());
        strategy->Allow(true);

        dist_lock::DistLockedTask locked_task(
            kWorkerName, [&] { work.Work(); }, strategy, MakeSettings(),
            dist_lock::DistLockWaitingMode::kNoWait);

        EXPECT_TRUE(work.WaitForLocked(true, kAttemptTimeout));

        work.SetWorkLoopOn(false);
        locked_task.WaitFor(kMaxTestWaitTime);

        EXPECT_TRUE(locked_task.GetState() == engine::Task::State::kCompleted);
        EXPECT_EQ(1u, work.GetFinishedWorkCount());
      },
      3);
}

TEST(LockedTask, MultipleWorkers) {
  RunInCoro([] {
    auto strategy = MakeMockStrategy();
    DistLockWorkload work;

    EXPECT_EQ(0, work.GetStartedWorkCount());
    EXPECT_EQ(0, work.GetFinishedWorkCount());
    strategy->Allow(true);

    dist_lock::DistLockedTask first(kWorkerName, [&] { work.Work(); }, strategy,
                                    MakeSettings());

    EXPECT_TRUE(work.WaitForLocked(true, kAttemptTimeout));
    EXPECT_EQ(1, work.GetStartedWorkCount());

    dist_lock::DistLockedTask second(kWorkerName, [&] { work.Work(); },
                                     strategy, MakeSettings(),
                                     dist_lock::DistLockWaitingMode::kNoWait);

    second.WaitFor(kAttemptTimeout);
    EXPECT_TRUE(second.GetState() == engine::Task::State::kCompleted);
    EXPECT_EQ(1, work.GetStartedWorkCount());

    work.SetWorkLoopOn(false);
    first.WaitFor(kMaxTestWaitTime);
    second.WaitFor(kMaxTestWaitTime);

    EXPECT_EQ(1, work.GetFinishedWorkCount());
  });
}
