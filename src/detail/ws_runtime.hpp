#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <libwebsockets.h>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace polymarket::detail {

// LWS 4.4+ can permanently clean up OpenSSL after its final TLS context.
// One idle context keeps other HTTP/crypto clients usable across reconnects.
inline bool keep_tls_initialized() {
	static lws_context* const keeper = [] {
		static const std::array<lws_protocols, 2> protocols{
			{{"polymarket-keeper", lws_callback_http_dummy, 0, 0, 0, nullptr, 0},
			 {nullptr, nullptr, 0, 0, 0, nullptr, 0}}};
		lws_context_creation_info info{};
		info.port = CONTEXT_PORT_NO_LISTEN;
		info.protocols = protocols.data();
		info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		return lws_create_context(&info);
	}();
	return keeper != nullptr;
}

// Callback-owned shutdown cannot join its own worker. Reap it before process exit.
class RetiredThreads {
public:
	static RetiredThreads& instance() {
		static RetiredThreads value;
		return value;
	}
	~RetiredThreads() { reap(true); }
	void adopt(std::thread thread, std::shared_future<void> finished) noexcept {
		try {
			const std::lock_guard lock(mutex_);
			threads_.reserve(threads_.size() + 1);
			threads_.push_back({std::move(thread), std::move(finished)});
		} catch (...) {
			if (thread.joinable())
				thread.detach();
		}
	}
	void reap(bool all = false) {
		std::vector<Record> ready;
		{
			const std::lock_guard lock(mutex_);
			if (all)
				ready.swap(threads_);
			else {
				ready.reserve(threads_.size());
				std::size_t kept = 0;
				for (std::size_t i = 0; i < threads_.size(); ++i) {
					if (threads_[i].finished.wait_for(std::chrono::seconds::zero()) ==
							std::future_status::ready &&
						threads_[i].thread.get_id() != std::this_thread::get_id())
						ready.push_back(std::move(threads_[i]));
					else {
						if (kept != i)
							threads_[kept] = std::move(threads_[i]);
						++kept;
					}
				}
				threads_.resize(kept);
			}
		}
		for (Record& record : ready)
			if (record.thread.joinable()) {
				if (record.thread.get_id() == std::this_thread::get_id())
					record.thread.detach();
				else
					record.thread.join();
			}
	}

private:
	std::mutex mutex_;
	struct Record {
		std::thread thread;
		std::shared_future<void> finished;
	};
	std::vector<Record> threads_;
};

struct WsRuntime {
	lws_context* context{nullptr};
	lws* wsi{nullptr}; // service thread after start()
	std::atomic<bool> connected{false};
	std::atomic<bool> should_stop{true};
	std::atomic<bool> worker_running{false};

	void wake() {
		const std::lock_guard lock(context_mutex_);
		if (context)
			lws_cancel_service(context);
	}
	void release_context() {
		lws_sul_cancel(&timer_.sul);
		lws_context* previous;
		{
			const std::lock_guard lock(context_mutex_);
			previous = std::exchange(context, nullptr);
			wsi = nullptr;
		}
		if (previous)
			lws_context_destroy(previous);
		timer_.sul = {};
	}
	void stop() {
		should_stop.store(true, std::memory_order_release);
		connected.store(false, std::memory_order_release);
		wake();
		std::thread previous;
		{
			const std::lock_guard lock(thread_mutex_);
			previous = std::move(thread_);
		}
		if (previous.joinable()) {
			if (previous.get_id() == std::this_thread::get_id())
				RetiredThreads::instance().adopt(std::move(previous), finished_);
			else
				previous.join();
		}
	}
	template <class State, class Tick>
	void start(const std::shared_ptr<State>& state, Tick tick) {
		// Initialize the reaper after OpenSSL so its exit handler joins workers first.
		(void)RetiredThreads::instance();
		const std::shared_ptr<std::latch> ready = std::make_shared<std::latch>(1);
		std::promise<void> completion;
		finished_ = completion.get_future().share();
		lws_context* const active = context;
		timer_.owner = this;
		lws_sul_schedule(active, 0, &timer_.sul, &timer_callback, 100000);
		worker_running.store(true, std::memory_order_release);
		try {
			const std::lock_guard lock(thread_mutex_);
			thread_ = std::thread(
				[state, ready, active, tick, completion = std::move(completion)]() mutable {
					ready->wait();
					try {
						while (!state->should_stop.load(std::memory_order_acquire)) {
							if (lws_service(active, 0) < 0)
								break;
							if (!state->should_stop.load(std::memory_order_acquire))
								tick(*state);
						}
					} catch (...) { /* Transport failures end this connection. */
					}
					state->connected.store(false, std::memory_order_release);
					state->release_context();
					state->worker_running.store(false, std::memory_order_release);
					completion.set_value_at_thread_exit();
				});
		} catch (...) {
			worker_running.store(false, std::memory_order_release);
			release_context();
			throw;
		}
		ready->count_down();
	}

private:
	struct Timer {
		lws_sorted_usec_list_t sul{};
		WsRuntime* owner{nullptr};
	} timer_;
	static void timer_callback(lws_sorted_usec_list_t* sul) {
		Timer* timer = reinterpret_cast<Timer*>(sul);
		WsRuntime& owner = *timer->owner;
		if (owner.should_stop.load(std::memory_order_acquire))
			return;
		owner.wake();
		lws_sul_schedule(owner.context, 0, sul, &timer_callback, 100000);
	}
	std::mutex context_mutex_;
	std::mutex thread_mutex_;
	std::thread thread_;
	std::shared_future<void> finished_;
};
} // namespace polymarket::detail
