// SPDX-License-Identifier: Apache-2.0
// Studio Live's session server (libs/live server) on hostile clients: the input is split at 0xFF into frames; the
// first byte of each picks one of three connections (a trusted host, two plain joiners), the rest is decoded as an
// envelope and fed to the server, with the clock moving and tick() between frames. Never a crash.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cc/live/envelope.hpp"
#include "cc/live/server.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  cc::live::ServerConfig cfg;
  cfg.app = "cadcraft";
  cfg.plain = true;
  cfg.auto_approve = true;
  cfg.log_keep = 50;
  cc::live::SessionServer s(cfg);
  cc::live::Ms now = 1000;
  s.open(1, true, "local", now);
  s.open(2, false, "lan", now);
  s.open(3, false, "relay", now);
  const std::span<const std::uint8_t> in(data, size);
  std::vector<std::uint8_t> frame;
  for (std::size_t i = 0; i <= in.size(); ++i) {
    if (i == in.size() || in[i] == 0xff) {
      if (!frame.empty()) {
        const cc::live::ConnId conn = 1 + (frame[0] % 3U);
        if (auto e = cc::live::decode(std::span(frame).subspan(1))) {
          (void)s.frame(conn, std::move(*e), now);
        } else if (frame[0] == 0xfe) {
          (void)s.closed(conn, now);
          s.open(conn, conn == 1, "lan", now);
        }
      }
      frame.clear();
      now += 17;
      (void)s.tick(now);
    } else {
      frame.push_back(in[i]);
    }
  }
  (void)s.export_state();
  return 0;
}
