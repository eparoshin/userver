#include "thread_pool_config.hpp"

USERVER_NAMESPACE_BEGIN

namespace engine::ev {

ThreadPoolConfig Parse(const yaml_config::YamlConfig& value,
                       formats::parse::To<ThreadPoolConfig>) {
  ThreadPoolConfig config;
  config.threads = value["threads"].As<std::size_t>(config.threads);
  config.dedicated_timer_threads =
      value["dedicated_timer_threads"].As<std::size_t>(
          config.dedicated_timer_threads);
  config.thread_name = value["thread_name"].As<std::string>(config.thread_name);
  return config;
}

}  // namespace engine::ev

USERVER_NAMESPACE_END
