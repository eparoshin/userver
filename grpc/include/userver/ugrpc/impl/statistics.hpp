#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <grpcpp/support/status.h>

#include <userver/utils/fixed_array.hpp>
#include <userver/utils/statistics/fwd.hpp>
#include <userver/utils/statistics/percentile.hpp>
#include <userver/utils/statistics/rate_counter.hpp>
#include <userver/utils/statistics/recentperiod.hpp>

#include <userver/ugrpc/impl/static_metadata.hpp>

USERVER_NAMESPACE_BEGIN

namespace utils::statistics {
class StripedRateCounter;
}  // namespace utils::statistics

namespace ugrpc::impl {

enum class StatisticsDomain { kClient, kServer };

std::string_view ToString(StatisticsDomain);

class MethodStatistics final {
 public:
  explicit MethodStatistics(
      StatisticsDomain domain,
      utils::statistics::StripedRateCounter& global_started);

  void AccountStarted() noexcept;

  void AccountStatus(grpc::StatusCode code) noexcept;

  void AccountTiming(std::chrono::milliseconds timing) noexcept;

  // All errors without gRPC status codes are categorized as "network errors".
  // See server::RpcInterruptedError.
  void AccountNetworkError() noexcept;

  // Occurs when the service forgot to finish a request, oftentimes due to a
  // thrown exception. Always indicates a programming error in our service.
  // UNKNOWN status code is automatically returned in this case.
  void AccountInternalError() noexcept;

  void AccountCancelledByDeadlinePropagation() noexcept;

  void AccountDeadlinePropagated() noexcept;

  void AccountCancelled() noexcept;

  friend void DumpMetric(utils::statistics::Writer& writer,
                         const MethodStatistics& stats);

  std::uint64_t GetStarted() const noexcept;

  void MoveStartedTo(MethodStatistics& other) noexcept;

 private:
  using Percentile =
      utils::statistics::Percentile<2000, std::uint32_t, 256, 100>;
  using Timings = utils::statistics::RecentPeriod<Percentile, Percentile>;
  using RateCounter = utils::statistics::RateCounter;
  // StatusCode enum cases have consecutive underlying values, starting from 0.
  // UNAUTHENTICATED currently has the largest value.
  static constexpr std::size_t kCodesCount =
      static_cast<std::size_t>(grpc::StatusCode::UNAUTHENTICATED) + 1;

  const StatisticsDomain domain_;
  utils::statistics::StripedRateCounter& global_started_;

  RateCounter started_{0};
  RateCounter started_renamed_{0};
  std::array<RateCounter, kCodesCount> status_codes_{};
  Timings timings_;
  RateCounter network_errors_{0};
  RateCounter internal_errors_{0};
  RateCounter cancelled_{0};

  RateCounter deadline_updated_{0};
  RateCounter deadline_cancelled_{0};
};

class ServiceStatistics final {
 public:
  ServiceStatistics(const StaticServiceMetadata& metadata,
                    StatisticsDomain domain,
                    utils::statistics::StripedRateCounter& global_started);

  ~ServiceStatistics();

  MethodStatistics& GetMethodStatistics(std::size_t method_id);
  const MethodStatistics& GetMethodStatistics(std::size_t method_id) const;

  const StaticServiceMetadata& GetMetadata() const;

  std::uint64_t GetStartedRequests() const;

  friend void DumpMetric(utils::statistics::Writer& writer,
                         const ServiceStatistics& stats);

 private:
  const StaticServiceMetadata metadata_;
  utils::FixedArray<MethodStatistics> method_statistics_;
};

}  // namespace ugrpc::impl

USERVER_NAMESPACE_END
