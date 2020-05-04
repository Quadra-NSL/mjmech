// Copyright 2020 Josh Pieper, jjp@pobox.com.
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

#include "mech/rpi3_hat_aux_stm32.h"

#include <sched.h>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <fmt/format.h>

#include "mjlib/base/assert.h"
#include "mjlib/base/fail.h"
#include "mjlib/base/json5_write_archive.h"
#include "mjlib/base/string_span.h"
#include "mjlib/base/system_error.h"
#include "mjlib/io/now.h"
#include "mjlib/io/repeating_timer.h"

#include "base/logging.h"
#include "base/saturate.h"

#include "mech/rpi3_raw_spi.h"

namespace mjmech {
namespace mech {

namespace pl = std::placeholders;

namespace {
/// This is the format exported by register 34 on the hat.
struct DeviceAttitudeData {
  uint8_t present = 0;
  uint8_t update_time_10us = 0;
  float w = 0;
  float x = 0;
  float y = 0;
  float z = 0;
  float x_dps = 0;
  float y_dps = 0;
  float z_dps = 0;
  float a_x_mps2 = 0;
  float a_y_mps2 = 0;
  float a_z_mps2 = 0;
  float bias_x_dps = 0;
  float bias_y_dps = 0;
  float bias_z_dps = 0;
  float uncertainty_w = 0;
  float uncertainty_x = 0;
  float uncertainty_y = 0;
  float uncertainty_z = 0;
  float uncertainty_bias_x_dps = 0;
  float uncertainty_bias_y_dps = 0;
  float uncertainty_bias_z_dps = 0;
  uint8_t padding[4] = {};
} __attribute__((packed));

struct DeviceMountingAngle {
  float yaw_deg = 0;
  float pitch_deg = 0;
  float roll_deg = 0;

  bool operator==(const DeviceMountingAngle& rhs) const {
    return yaw_deg == rhs.yaw_deg &&
        pitch_deg == rhs.pitch_deg &&
        roll_deg == rhs.roll_deg;
  }

  bool operator!=(const DeviceMountingAngle& rhs) const {
    return !(*this == rhs);
  }
} __attribute__((packed));
}

class Rpi3HatAuxStm32::Impl {
 public:
  Impl(const boost::asio::executor& executor, const Options& options)
      : executor_(executor),
        options_(options) {
    thread_ = std::thread(std::bind(&Impl::Run, this));
  }

  ~Impl() {
    child_context_.stop();
    thread_.join();
  }

  void AsyncStart(mjlib::io::ErrorCallback callback) {
    timer_.start(base::ConvertSecondsToDuration(
                     options_.power_poll_period_s),
                 std::bind(&Impl::HandleTimer, this, pl::_1));
    boost::asio::post(
        executor_,
        std::bind(std::move(callback), mjlib::base::error_code()));
  }

  void ReadImu(AttitudeData* data, mjlib::io::ErrorCallback callback) {
    imu_read_outstanding_.store(true);
    boost::asio::post(
        child_context_,
        [this, data, callback=std::move(callback)]() mutable {
          this->Child_ReadImu(data, std::move(callback));
        });
  }

  void AsyncWaitForSlot(int* remote,
                        uint16_t* bitfield,
                        mjlib::io::ErrorCallback callback) {
    rf_read_outstanding_.store(true);
    boost::asio::post(
        child_context_,
        [this, remote, bitfield, callback=std::move(callback)]() mutable {
          this->Child_WaitForSlot(remote, bitfield, std::move(callback));
        });
  }

  Slot rx_slot(int remote, int slot_idx) {
    MJ_ASSERT(remote == 0);
    std::unique_lock lock(slot_mutex_);
    return rx_slots_[slot_idx];
  }

  void tx_slot(int remote, int slot_idx, const Slot& slot) {
    MJ_ASSERT(remote == 0);
    std::unique_lock lock(slot_mutex_);
    tx_slots_[slot_idx] = slot;
    boost::asio::post(
        child_context_,
        [this, slot_idx, slot]() {
          this->Child_WriteSlot(slot_idx, slot);
        });
  }

  Slot tx_slot(int remote, int slot_idx) {
    MJ_ASSERT(remote == 0);
    std::unique_lock lock(slot_mutex_);
    return tx_slots_[slot_idx];
  }

  void HandleTimer(const mjlib::base::error_code& ec) {
    mjlib::base::FailIf(ec);

    boost::asio::post(
        child_context_,
        [this]() {
          this->Child_KickPowerDist();
        });
  }

 private:
  void Run() {
    {
      struct sched_param param = {};
      param.sched_priority = 99;

      mjlib::base::system_error::throw_if(
          ::sched_setscheduler(0, SCHED_RR, &param) < 0,
          "error setting real time");
    }
    if (options_.cpu_affinity >= 0) {
      cpu_set_t cpuset = {};
      CPU_ZERO(&cpuset);
      CPU_SET(options_.cpu_affinity, &cpuset);

      mjlib::base::system_error::throw_if(
          ::sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) < 0,
          "error setting affinity");

      std::cout << fmt::format(
          "Rpi3HatAuxStm32 cpu affinity set to {}\n", options_.cpu_affinity);
    }

    spi_ = std::make_unique<Rpi3RawSpi>([&]() {
        Rpi3RawSpi::Options options;
        options.speed_hz = options_.speed;
        return options;
      }());

    // Configure our mounting angle.
    DeviceMountingAngle device_mounting;
    device_mounting.yaw_deg = options_.mounting.yaw_deg;
    device_mounting.pitch_deg = options_.mounting.pitch_deg;
    device_mounting.roll_deg = options_.mounting.roll_deg;

    spi_->Write(0, 36, std::string_view(
                    reinterpret_cast<const char*>(&device_mounting),
                    sizeof(device_mounting)));
    // Give it some time to work.
    ::usleep(100);
    DeviceMountingAngle mounting_verify;
    spi_->Read(0, 35, mjlib::base::string_span(
                   reinterpret_cast<char*>(&mounting_verify),
                   sizeof(mounting_verify)));
    mjlib::base::system_error::throw_if(
        device_mounting != mounting_verify,
        fmt::format("Mounting angle not set properly ({},{},{}) != ({},{},{})",
                    device_mounting.yaw_deg,
                    device_mounting.pitch_deg,
                    device_mounting.roll_deg,
                    mounting_verify.yaw_deg,
                    mounting_verify.pitch_deg,
                    mounting_verify.roll_deg));

    // Configure our RF id.
    spi_->Write(0, 50, std::string_view(
                    reinterpret_cast<const char*>(&options_.rf_id),
                    sizeof(uint32_t)));
    ::usleep(100);
    uint32_t id_verify = 0;
    spi_->Read(0, 49, mjlib::base::string_span(
                   reinterpret_cast<char*>(&id_verify), sizeof(id_verify)));
    mjlib::base::system_error::throw_if(
        options_.rf_id != id_verify,
        fmt::format("RF Id not set properly ({} != {})",
                    options_.rf_id,
                    id_verify));

    boost::asio::io_context::work work(child_context_);
    child_context_.run();
  }

  void Child_WriteSlot(int slot_idx, const Slot& slot) {
    if (child_priorities_[slot_idx] != slot.priority) {
      child_priorities_[slot_idx] = slot.priority;
      uint8_t buf[5] = {};
      buf[0] = slot_idx;
      std::memcpy(&buf[1], &slot.priority, sizeof(slot.priority));
      spi_->Write(0, 53, std::string_view(
                      reinterpret_cast<const char*>(&buf[0]), sizeof(buf)));
    }

    uint8_t buf[17] = {};
    buf[0] = slot_idx;
    std::memcpy(&buf[1], slot.data, slot.size);
    spi_->Write(0, 52, std::string_view(
                    reinterpret_cast<const char*>(&buf[0]), slot.size + 1));
  }

  struct SlotData {
    uint32_t age_ms = 0;
    uint8_t size;
    char data[15] = {};
  } __attribute__((packed));

  void Child_WaitForSlot(int* remote, uint16_t* bitfield,
                         mjlib::io::ErrorCallback callback) {
    // We don't support multiple remotes.
    *remote = 0;

    // We'll busy loop, and re-queue ourselves if IMU data is
    // available and we have a request outstanding.
    decltype(Child_PollData()) result = {};

    while (true) {
      result = Child_PollData();
      if (result.can) {
        Child_ReadCan();
      }
      if (result.rf_bitfield_delta) {
        break;
      }
      if (result.imu && imu_read_outstanding_.load()) {
        boost::asio::post(
            child_context_,
            [this, remote, bitfield, callback=std::move(callback)]() mutable {
              Child_WaitForSlot(remote, bitfield, std::move(callback));
            });
        return;
      }
    }

    last_bitfield_ ^= result.rf_bitfield_delta;

    *bitfield = 0;

    // Read any slots that need reading.
    for (int i = 0; i < 15; i++) {
      if ((result.rf_bitfield_delta & (3 << (i * 2))) == 0) {
        continue;
      }

      *bitfield |= (1 << i);
      SlotData slot_data;
      spi_->Read(0, 64 + i, mjlib::base::string_span(
                     reinterpret_cast<char*>(&slot_data), sizeof(slot_data)));

      std::unique_lock lock(slot_mutex_);
      rx_slots_[i].timestamp = mjlib::io::Now(child_context_);
      rx_slots_[i].size = slot_data.size;
      std::memcpy(&rx_slots_[i].data[0], slot_data.data, 16);
    }

    rf_read_outstanding_.store(false);

    boost::asio::post(
        executor_,
        std::bind(std::move(callback), mjlib::base::error_code()));
  }

  void Child_ReadImu(AttitudeData* data, mjlib::io::ErrorCallback callback) {
    while (true) {
      const auto result = Child_PollData();
      if (result.can) {
        Child_ReadCan();
      }
      if (result.imu) { break; }
      if (result.rf_bitfield_delta && rf_read_outstanding_.load()) {
        // We'll re-enqueue ourselves so that the RF read can
        // complete.
        boost::asio::post(
            child_context_,
            [this, data, callback=std::move(callback)]() mutable {
              Child_ReadImu(data, std::move(callback));
            });
        return;
      }
    }

    imu_read_outstanding_.store(false);

    // We should have something for sure at this point, but just in
    // case busy loop.
    //
    // TODO(jpieper): Switch to using the IRQ when it is available.
    while (true) {
      device_data_ = {};

      while ((device_data_.present & 0x01) == 0) {
        spi_->Read(0, 34, mjlib::base::string_span(
                       reinterpret_cast<char*>(&device_data_), sizeof(device_data_)));
      }

      data->timestamp = mjlib::io::Now(child_context_);
      const auto& dd = device_data_;
      data->attitude = { dd.w, dd.x, dd.y, dd.z };
      data->rate_dps = { dd.x_dps, dd.y_dps, dd.z_dps };
      data->euler_deg = (180.0 / M_PI) * data->attitude.euler_rad();
      data->accel_mps2 = { dd.a_x_mps2, dd.a_y_mps2, dd.a_z_mps2 };
      data->bias_dps = { dd.bias_x_dps, dd.bias_y_dps, dd.bias_z_dps };
      data->attitude_uncertainty = {
        dd.uncertainty_w,
        dd.uncertainty_x,
        dd.uncertainty_y,
        dd.uncertainty_z,
      };
      data->bias_uncertainty_dps = {
        dd.uncertainty_bias_x_dps,
        dd.uncertainty_bias_y_dps,
        dd.uncertainty_bias_z_dps,
      };

      if (std::abs(data->attitude.norm() - 1.0) > 1.0) {
        if (0) {
          // This data is probably corrupt.  Exit now so that we can look
          // at the problem on a scope.
          std::cerr << "Corrupt IMU data received!\n";
          for (size_t i = 0; i < sizeof(device_data_); i++) {
            std::cerr << fmt::format(
                "{:02x} ",
                reinterpret_cast<const uint8_t*>(&device_data_)[i]);
          }
          std::cerr << "\n";
          std::cerr << mjlib::base::Json5WriteArchive::Write(*data) << "\n";

          std::exit(1);
        } else {
          // Lets just try this again.
          continue;
        }
      }
      break;
    }

    boost::asio::post(
        executor_,
        std::bind(std::move(callback), mjlib::base::error_code()));
  }

  struct PollResult {
    bool imu = false;
    uint32_t rf_bitfield_delta = 0;
    bool can = false;
  };

  void Child_KickPowerDist() {
    char buf[8] = {};
    buf[0] = 1;  // CAN bus 1
    buf[1] = 0x00; // ID = 0x00010005
    buf[2] = 0x01;
    buf[3] = 0x00;
    buf[4] = 0x05;
    buf[5] = 2;  // size=2
    buf[6] = 0;  // ignored
    buf[7] = base::Saturate<uint8_t>(options_.shutdown_timeout_s / 0.1);
    spi_->Write(0, 18, std::string_view(buf, sizeof(buf)));
  }

  PollResult Child_PollData() {
    struct Data {
      uint8_t can_queue = 0;
      uint8_t imu_present = 0;
      uint32_t rf_bitfield = 0;
    } __attribute__((packed));
    Data data;

    spi_->Read(0, 96, mjlib::base::string_span(
                   reinterpret_cast<char*>(&data), sizeof(data)));
    PollResult result;
    result.imu = data.imu_present != 0;
    result.rf_bitfield_delta = data.rf_bitfield ^ last_bitfield_;
    result.can = data.can_queue != 0;

    return result;
  }

  void Child_ReadCan() {
    char buf[70] = {};
    spi_->Read(0, 17, buf);

    // buf[0] - we don't care which bus this came from

    const auto u8 = [](auto v) { return static_cast<uint8_t>(v); };

    const uint32_t id = (
        (u8(buf[1]) << 24) |
        (u8(buf[2]) << 16) |
        (u8(buf[3]) << 8) |
        (u8(buf[4])));

    if (id != 0x00010004) { return; }
    if (buf[5] < 2) { return; }

    const bool power_switch = buf[6] != 0;
    // const int remaining_tenth_seconds = u8(buf[7]);

    if (!power_switch) {
      // The power switch is off.  Shut everything down!
      log_.warn("Shutting down!");
      mjlib::base::system_error::throw_if(
          ::system("shutdown -h now") < 0,
          "error trying to shutdown, at least we'll exit the app");
      // Quit the application to make the shutdown process go faster.
      std::exit(0);
    }
  }

  base::LogRef log_ = base::GetLogInstance("Rpi3HatAuxStm32");

  boost::asio::executor executor_;
  const Options options_;

  mjlib::io::RepeatingTimer timer_{executor_};

  std::thread thread_;

  // Only accessed from child thread.
  boost::asio::io_context child_context_;
  std::unique_ptr<Rpi3RawSpi> spi_;
  DeviceAttitudeData device_data_;
  uint32_t last_bitfield_ = 0;
  uint32_t child_priorities_[15] = {};

  // Accessed from both.
  std::atomic<bool> imu_read_outstanding_{false};
  std::atomic<bool> rf_read_outstanding_{false};

  std::mutex slot_mutex_;
  std::array<Slot, 15> rx_slots_;
  std::array<Slot, 15> tx_slots_;
};

Rpi3HatAuxStm32::Rpi3HatAuxStm32(
    const boost::asio::executor& executor, const Options& options)
    : impl_(std::make_unique<Impl>(executor, options)) {}

Rpi3HatAuxStm32::~Rpi3HatAuxStm32() {}

void Rpi3HatAuxStm32::AsyncStart(mjlib::io::ErrorCallback callback) {
  impl_->AsyncStart(std::move(callback));
}

void Rpi3HatAuxStm32::ReadImu(AttitudeData* data,
                              mjlib::io::ErrorCallback callback) {
  impl_->ReadImu(data, std::move(callback));
}

void Rpi3HatAuxStm32::AsyncWaitForSlot(
    int* remote,
    uint16_t* bitfield,
    mjlib::io::ErrorCallback callback) {
  impl_->AsyncWaitForSlot(remote, bitfield, std::move(callback));
}

Rpi3HatAuxStm32::Slot Rpi3HatAuxStm32::rx_slot(int remote, int slot_idx) {
  return impl_->rx_slot(remote, slot_idx);
}

void Rpi3HatAuxStm32::tx_slot(int remote, int slot_idx, const Slot& slot) {
  impl_->tx_slot(remote, slot_idx, slot);
}

Rpi3HatAuxStm32::Slot Rpi3HatAuxStm32::tx_slot(int remote, int slot_idx) {
  return impl_->tx_slot(remote, slot_idx);
}

}
}
