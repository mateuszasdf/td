//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/net/DcId.h"

#include "td/mtproto/RSA.h"

#include "td/utils/common.h"
#include "td/utils/port/RwMutex.h"
#include "td/utils/Status.h"

namespace td {

class PublicRsaKeyShared final : public mtproto::PublicRsaKeyInterface {
 public:
  PublicRsaKeyShared(DcId dc_id, bool is_test);

  class Listener {
   public:
    Listener() = default;
    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;
    Listener(Listener &&) = delete;
    Listener &operator=(Listener &&) = delete;
    virtual ~Listener() = default;
    virtual bool notify() = 0;
  };

  void add_rsa(mtproto::RSA rsa);
  Result<RsaKey> get_rsa_key(const vector<int64> &fingerprints) final;
  void drop_keys() final;
  bool has_keys();

  void add_listener(unique_ptr<Listener> listener);

  DcId dc_id() const {
    return dc_id_;
  }

 private:
  DcId dc_id_;
  struct RsaOption {
    int64 fingerprint;
    mtproto::RSA rsa;
  };
  std::vector<RsaOption> options_;
  std::vector<unique_ptr<Listener>> listeners_;
  RwMutex rw_mutex_;

  mtproto::RSA *get_rsa_unsafe(int64 fingerprint);

  void notify();
};

}  // namespace td
