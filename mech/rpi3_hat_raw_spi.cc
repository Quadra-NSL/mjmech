// Copyright 2019-2020 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mech/rpi3_hat_raw_spi.h"

#include <sched.h>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <future>
#include <optional>
#include <sstream>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <fmt/format.h>

#include "mjlib/base/fail.h"
#include "mjlib/io/deadline_timer.h"
#include "mjlib/multiplex/frame.h"

#include "mech/rpi3_raw_aux_spi.h"

namespace mjmech {
namespace mech {

namespace {
auto u32 = [](auto v) { return static_cast<uint32_t>(v); };
auto i64 = [](auto v) { return static_cast<int64_t>(v); };

static size_t RoundUpDlc(size_t value) {
  if (value == 0) { return 0; }
  if (value == 1) { return 1; }
  if (value == 2) { return 2; }
  if (value == 3) { return 3; }
  if (value == 4) { return 4; }
  if (value == 5) { return 5; }
  if (value == 6) { return 6; }
  if (value == 7) { return 7; }
  if (value == 8) { return 8; }
  if (value <= 12) { return 12; }
  if (value <= 16) { return 16; }
  if (value <= 20) { return 20; }
  if (value <= 24) { return 24; }
  if (value <= 32) { return 32; }
  if (value <= 48) { return 48; }
  if (value <= 64) { return 64; }
  return 0;
}

int64_t GetNow() {
  struct timespec ts = {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return i64(ts.tv_sec) * 1000000000ll + i64(ts.tv_nsec);
}

void ThrowIf(bool value, std::string_view message = "") {
  if (value) {
    throw mjlib::base::system_error::syserrno(message.data());
  }
}

struct FrameItem {
  mjlib::multiplex::Frame frame;
  char encoded[256] = {};
  std::size_t size = 0;
};
}

class Rpi3HatRawSpi::Impl {
 public:
  Impl(const boost::asio::executor& executor,
       const Options& options)
      : executor_(executor),
        options_(options) {
    Start();
  }

  ~Impl() {
    child_context_.stop();
    thread_.join();
  }

  void Start() {
    thread_ = std::thread(std::bind(&Impl::Run, this));
    startup_promise_.set_value(true);
  }

  void AsyncStart(mjlib::io::ErrorCallback callback) {
    boost::asio::post(
        executor_,
        std::bind(std::move(callback), mjlib::base::error_code()));
  }

  void AsyncRegister(const IdRequest& request,
                     SingleReply* reply,
                     mjlib::io::ErrorCallback callback) {
    AssertParent();
    single_request_ = { request };
    single_reply_ = {};

    auto continuation =
        [this, reply, callback=std::move(callback)](const auto& ec) mutable {
      mjlib::base::FailIf(ec);

      BOOST_ASSERT(single_reply_.replies.size() == 1);
      *reply = single_reply_.replies.front();

      callback(ec);
    };

    boost::asio::post(
        child_context_,
        [this, continuation=std::move(continuation)]() mutable {
          this->ThreadAsyncRegister(&single_request_, &single_reply_,
                                    std::move(continuation));
        });
  }

  void AsyncRegisterMultiple(
      const std::vector<IdRequest>& request,
      Reply* reply,
      mjlib::io::ErrorCallback callback) {
    AssertParent();

    boost::asio::post(
        child_context_,
        [this, callback=std::move(callback), &request, reply]() mutable {
          this->ThreadAsyncRegister(&request, reply, std::move(callback));
        });
  }

  class Tunnel : public mjlib::io::AsyncStream,
                 public std::enable_shared_from_this<Tunnel> {
   public:
    Tunnel(Impl* parent, uint8_t id, uint32_t channel,
           const TunnelOptions& options)
        : parent_(parent),
          id_(id),
          channel_(channel),
          options_(options) {}

    ~Tunnel() override {}

    void async_read_some(mjlib::io::MutableBufferSequence buffers,
                         mjlib::io::ReadHandler handler) override {
      boost::asio::post(
          parent_->child_context_,
          [self=shared_from_this(), buffers,
           handler=std::move(handler)]() mutable {
            const auto bytes_read =
                self->parent_->ThreadTunnelPoll(
                    self->id_, self->channel_, buffers);
            if (bytes_read > 0) {
              boost::asio::post(
                  self->parent_->executor_,
                  [self, handler=std::move(handler), bytes_read]() mutable {
                    handler(mjlib::base::error_code(), bytes_read);
                  });
            } else {
              self->timer_.expires_from_now(self->options_.poll_rate);
              self->timer_.async_wait(
                  [self, handler=std::move(handler), buffers](
                      const auto& ec) mutable {
                    self->HandlePoll(ec, buffers, std::move(handler));
                  });
            }
          });
    }

    void async_write_some(mjlib::io::ConstBufferSequence buffers,
                          mjlib::io::WriteHandler handler) override {
      boost::asio::post(
          parent_->child_context_,
          [self=shared_from_this(), buffers, handler=std::move(handler)]() mutable {
            self->parent_->ThreadTunnelWrite(
                self->id_, self->channel_, buffers, std::move(handler));
          });
    }

    boost::asio::executor get_executor() override {
      return parent_->executor_;
    }

    void cancel() override {
      timer_.cancel();
    }

   private:
    void HandlePoll(const mjlib::base::error_code& ec,
                    mjlib::io::MutableBufferSequence buffers,
                    mjlib::io::ReadHandler handler) {
      if (ec) {
        handler(ec, 0);
        return;
      }

      async_read_some(buffers, std::move(handler));
    }

    Impl* const parent_;
    const uint8_t id_;
    const uint32_t channel_;
    const TunnelOptions options_;

    mjlib::io::DeadlineTimer timer_{parent_->executor_};
  };

  mjlib::io::SharedStream MakeTunnel(
      uint8_t id,
      uint32_t channel,
      const TunnelOptions& options) {
    return std::make_shared<Tunnel>(this, id, channel, options);
  }

 private:
  void Run() {
    startup_future_.get();
    AssertThread();

    if (options_.cpu_affinity >= 0) {
      cpu_set_t cpuset = {};
      CPU_ZERO(&cpuset);
      CPU_SET(options_.cpu_affinity, &cpuset);

      ThrowIf(::sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) < 0);
      std::cout << fmt::format(
          "Rpi3HatRawSpi cpu affinity set to: {}\n", options_.cpu_affinity);
    }

    // TODO: Set realtime priority and lock memory.

    // Open devices.
    spi_ = std::make_unique<Rpi3RawAuxSpi>([&]() {
        Rpi3RawAuxSpi::Options options;
        options.speed_hz = options_.spi_speed_hz;
        return options;
      }());

    boost::asio::io_context::work work(child_context_);
    child_context_.run();
  }

  size_t ThreadTunnelPoll(uint8_t id, uint32_t channel,
                          mjlib::io::MutableBufferSequence buffers) {
    AssertThread();

    const int cs = (id <= 6) ? 0 : 1;

    // Before we ask for more, make sure there isn't anything stale
    // lying around.
    {
      FrameItem item;
      ReadFrame(cs, &item, true);
      if (item.size) {
        return ParseTunnelPoll(&item, channel, buffers);
      }
    }

    mjlib::base::FastOStringStream stream;
    mjlib::multiplex::WriteStream writer{stream};

    writer.WriteVaruint(
        u32(mjlib::multiplex::Format::Subframe::kClientPollServer));
    writer.WriteVaruint(channel);
    writer.WriteVaruint(
        std::min<uint32_t>(48, boost::asio::buffer_size(buffers)));

    WriteCanFrame(0, id, kRequestReply, stream.str());

    {
      FrameItem item;
      ReadFrame(cs, &item);

      return ParseTunnelPoll(&item, channel, buffers);
    }
  }

  size_t ParseTunnelPoll(const FrameItem* item,
                         uint32_t channel,
                         mjlib::io::MutableBufferSequence buffers) {
    AssertThread();

    mjlib::base::BufferReadStream buffer_stream(
        {item->encoded, item->size});
    mjlib::multiplex::ReadStream<
      mjlib::base::BufferReadStream> stream{buffer_stream};

    stream.Read<uint8_t>();  // ignore which can bus it came in on
    stream.Read<uint16_t>();  // the top 16 bits of the CAN ID are ignored
    const auto maybe_source = stream.Read<uint8_t>();
    const auto maybe_dest = stream.Read<uint8_t>();
    const auto packet_size = stream.Read<uint8_t>();

    if (!maybe_source || !maybe_dest || !packet_size) {
      malformed_++;
      return 0;
    }

    const auto maybe_subframe = stream.ReadVaruint();
    if (!maybe_subframe || *maybe_subframe !=
        u32(mjlib::multiplex::Format::Subframe::kServerToClient)) {
      malformed_++;
      return 0;
    }

    const auto maybe_channel = stream.ReadVaruint();
    if (!maybe_channel || *maybe_channel != channel) {
      malformed_++;
      return 0;
    }

    const auto maybe_stream_size = stream.ReadVaruint();
    if (!maybe_stream_size) {
      malformed_++;
      return 0;
    }

    const auto stream_size = *maybe_stream_size;

    auto remaining_data = stream_size;
    size_t bytes_read = 0;
    for (auto buffer : buffers) {
      if (remaining_data == 0) { break; }

      const auto to_read = std::min<size_t>(buffer.size(), remaining_data);
      buffer_stream.read({static_cast<char*>(buffer.data()),
              static_cast<std::streamsize>(to_read)});
      remaining_data -= to_read;
      bytes_read += to_read;
    }

    return bytes_read;
  }

  void ThreadTunnelWrite(uint8_t id, uint32_t channel,
                         mjlib::io::ConstBufferSequence buffers,
                         mjlib::io::WriteHandler callback) {
    AssertThread();

    mjlib::base::FastOStringStream stream;
    mjlib::multiplex::WriteStream writer{stream};

    writer.WriteVaruint(u32(mjlib::multiplex::Format::Subframe::kClientToServer));
    writer.WriteVaruint(channel);

    const auto size = std::min<size_t>(48, boost::asio::buffer_size(buffers));
    writer.WriteVaruint(size);
    auto remaining_size = size;
    for (auto buffer : buffers) {
      if (remaining_size == 0) { break; }
      const auto to_write = std::min(remaining_size, buffer.size());
      stream.write({static_cast<const char*>(buffer.data()), to_write});
      remaining_size -= to_write;
    }

    WriteCanFrame(0, id, kNoReply, stream.str());

    boost::asio::post(
        executor_,
        std::bind(std::move(callback), mjlib::base::error_code(), size));
  }

  void ReadFrame(int cs, FrameItem* frame_item, bool one_try = false) {
    const int64_t now = GetNow();
    const int64_t timeout = now + options_.query_timeout_s * 1000000000ll;

    auto record_timeout = [&]() {
      frame_item->size = 0;
      timeouts_++;
    };

    while (true) {
      frame_item->size = 0;

      if (GetNow() > timeout) {
        record_timeout();
        return;
      }

      uint8_t packet_sizes[1] = {};
      spi_->Read(cs, 16, mjlib::base::string_span(
                     reinterpret_cast<char*>(packet_sizes),
                     sizeof(packet_sizes)));

      if (packet_sizes[0] == 0) {
        if (one_try) {
          return;
        }
        continue;
      }

      spi_->Read(
          cs, 17, mjlib::base::string_span(&frame_item->encoded[0], packet_sizes[0]));
      frame_item->size = packet_sizes[0];

      if (frame_item->encoded[0] == 0) {
        // If necessary, you can uncomment this for debugging so that
        // the problematic frame will remain on a scope.

        // std::cout << "Received BAD frame\n";
        // std::exit(0);
        continue;
      }

      return;
    }
  }

  void ParseFrame(const FrameItem* frame_item, Reply* reply) {
    AssertThread();

    mjlib::base::BufferReadStream buffer_stream(
        {frame_item->encoded, frame_item->size});
    mjlib::multiplex::ReadStream<
      mjlib::base::BufferReadStream> stream{buffer_stream};

    stream.Read<uint8_t>();  // ignore which can bus it came in on
    stream.Read<uint16_t>();  // the top 16 bits of the CAN ID are ignored
    const auto maybe_source = stream.Read<uint8_t>();
    const auto maybe_dest = stream.Read<uint8_t>();
    const auto packet_size = stream.Read<uint8_t>();

    if (!maybe_source || !maybe_dest || !packet_size) {
      malformed_++;
      return;
    }

    SingleReply this_reply;
    this_reply.id = *maybe_source;

    mjlib::base::BufferReadStream payload_stream{
      {buffer_stream.position(), *packet_size}};
    this_reply.reply = mjlib::multiplex::ParseRegisterReply(payload_stream);

    reply->replies.push_back(std::move(this_reply));
  }

  void ThreadAsyncRegister(const std::vector<IdRequest>* requests,
                           Reply* reply,
                           mjlib::io::ErrorCallback callback) {
    int bus_replies[2] = {0, 0};

    // Send out all our packets, then after read any replies.
    for (size_t i = 0; i < requests->size(); i++) {
      const auto& request = (*requests)[i];

      const int bus =
          WriteCanFrame(
              0,
              request.id,
              request.request.request_reply() ? kRequestReply : kNoReply,
              request.request.buffer());
      if (request.request.request_reply()) { bus_replies[bus]++; }
    }

    const auto timeout = GetNow() + options_.query_timeout_s * 1000000000ll;

    FrameItem frame_item;

    // Now wait for the proper number of replies from each bus or
    // until our timeout.
    while (bus_replies[0] != 0 || bus_replies[1] != 0) {
      if (GetNow() > timeout) {
        timeouts_++;
        break;
      }

      for (int bus = 0; bus < 2; bus++) {
        if (bus_replies[bus] == 0) { continue; }

        char queue_sizes[6] = {};
        spi_->Read(bus, 16, queue_sizes);

        // We're going to read everything, even if it is more than we
        // wanted.
        for (const int size : queue_sizes) {
          if (size == 0) { continue; }

          spi_->Read(bus, 17, mjlib::base::string_span(
                         &frame_item.encoded[0], size));
          frame_item.size = size;

          if (bus_replies[bus] > 0) { bus_replies[bus]--; }

          ParseFrame(&frame_item, reply);
        }
      }
    }

    boost::asio::post(
        executor_,
        std::bind(std::move(callback), mjlib::base::error_code()));
  }

  enum ReplyType {
    kNoReply,
    kRequestReply,
  };

  int WriteCanFrame(uint8_t source_id, uint8_t dest_id,
                    ReplyType reply_type, std::string_view data) {
    MJ_ASSERT(data.size() <= 64);

    const int cs = (dest_id <= 6) ? 0 : 1;
    const int bus = (((dest_id - 1) % 6) < 3) ? 1 : 2;

    auto size = RoundUpDlc(data.size());

    char buf[70] = {};
    MJ_ASSERT((6 + size) <= sizeof(buf));
    buf[0] = bus;
    buf[3] = source_id | (reply_type == kRequestReply) ? 0x80 : 00;
    buf[4] = dest_id;
    buf[5] = size;
    std::memcpy(&buf[6], data.data(), data.size());
    for (std::size_t i = 6 + data.size(); i < 6 + size; i++) {
      buf[i] = 0x50;
    }
    spi_->Write(cs, 18, std::string_view(buf, 6 + size));

    return cs;
  }

  void AssertThread() {
    MJ_ASSERT(std::this_thread::get_id() == thread_.get_id());
  }

  void AssertParent() {
    MJ_ASSERT(std::this_thread::get_id() != thread_.get_id());
  }

  boost::asio::executor executor_;
  boost::asio::executor_work_guard<boost::asio::executor> guard_{executor_};
  const Options options_;

  std::thread thread_;

  // Accessed from both.
  std::promise<bool> startup_promise_;
  std::future<bool> startup_future_ = startup_promise_.get_future();

  std::atomic<uint64_t> timeouts_{0};
  std::atomic<uint64_t> malformed_{0};

  std::vector<IdRequest> single_request_;
  Reply single_reply_;

  // Only accessed from the child thread.
  boost::asio::io_context child_context_;
  std::unique_ptr<Rpi3RawAuxSpi> spi_;
};

Rpi3HatRawSpi::Rpi3HatRawSpi(const boost::asio::executor& executor,
                             const Options& options)
    : impl_(std::make_unique<Impl>(executor, options)) {}

Rpi3HatRawSpi::~Rpi3HatRawSpi() {}

void Rpi3HatRawSpi::AsyncStart(mjlib::io::ErrorCallback callback) {
  impl_->AsyncStart(std::move(callback));
}

void Rpi3HatRawSpi::AsyncRegister(const IdRequest& request,
                                  SingleReply* reply,
                                  mjlib::io::ErrorCallback callback) {
  impl_->AsyncRegister(request, reply, std::move(callback));
}

void Rpi3HatRawSpi::AsyncRegisterMultiple(
    const std::vector<IdRequest>& request,
    Reply* reply,
    mjlib::io::ErrorCallback callback) {
  impl_->AsyncRegisterMultiple(request, reply, std::move(callback));
}

mjlib::io::SharedStream Rpi3HatRawSpi::MakeTunnel(
    uint8_t id,
    uint32_t channel,
    const TunnelOptions& options) {
  return impl_->MakeTunnel(id, channel, options);
}

Rpi3HatRawSpi::Stats Rpi3HatRawSpi::stats() const {
  return Stats();
}

}
}
