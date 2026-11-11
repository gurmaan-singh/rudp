#include "rudp/connection.hpp"

#include <algorithm>

namespace rudp {

Connection::Connection(Role role, const Config& config, std::uint64_t seed)
    : role_(role),
      cfg_(config),
      rng_(seed),
      cwnd_(config.initial_cwnd_segments * config.mss),
      rtt_(config.initial_rto, config.min_rto, config.max_rto),
      rto_(config.initial_rto),
      persist_interval_(config.min_rto) {}

// ---------------------------------------------------------------- application side

void Connection::connect(Time now) {
  if (role_ != Role::Client || phase_ != Phase::Idle) return;
  iss_ = cfg_.initial_seq ? *cfg_.initial_seq : static_cast<std::uint32_t>(rng_());
  snd_una_ = iss_;
  snd_nxt_ = iss_ + 1;
  Segment syn;
  syn.seq = iss_;
  syn.len = 1;
  syn.flags = kSyn;
  inflight_.push_back(std::move(syn));
  lost_count_ = 1;
  phase_ = Phase::SynSent;
  (void)now;
}

std::size_t Connection::write(std::span<const std::uint8_t> data) {
  if (fin_queued_ || phase_ == Phase::Failed) return 0;
  const std::size_t n = std::min(data.size(), writable());
  send_q_.insert(send_q_.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
  return n;
}

std::size_t Connection::read(std::span<std::uint8_t> out) {
  const std::size_t n = std::min(out.size(), recv_buf_.size());
  std::copy_n(recv_buf_.begin(), n, out.begin());
  recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + static_cast<std::ptrdiff_t>(n));
  stats_.bytes_delivered += n;
  // Tell the sender when a useful amount of room has opened up, or it may wait on a stale zero window.
  if (free_window() >= last_advertised_ + cfg_.mss) ack_pending_ = true;
  return n;
}

void Connection::close() { fin_queued_ = true; }

// ---------------------------------------------------------------- incoming

void Connection::on_datagram(std::span<const std::uint8_t> datagram, Time now) {
  clock_ = now;
  const auto dec = decode(datagram);
  if (!dec) {
    ++stats_.bad_packets;
    return;
  }
  ++stats_.packets_received;
  const Header& h = dec->header;
  if (phase_ == Phase::Failed) return;
  if (h.ts_val != 0) last_ts_rx_ = h.ts_val;

  if (phase_ == Phase::Idle) {
    // A listening server waits for a SYN.
    if (role_ == Role::Server && (h.flags & kSyn) && !(h.flags & kAck)) {
      irs_ = h.seq;
      rcv_nxt_ = irs_ + 1;
      rwnd_ = h.window;
      iss_ = cfg_.initial_seq ? *cfg_.initial_seq : static_cast<std::uint32_t>(rng_());
      snd_una_ = iss_;
      snd_nxt_ = iss_ + 1;
      Segment synack;
      synack.seq = iss_;
      synack.len = 1;
      synack.flags = kSyn;
      inflight_.push_back(std::move(synack));
      lost_count_ = 1;
      phase_ = Phase::SynRcvd;
    }
    return;
  }

  if (phase_ == Phase::SynSent) {
    if ((h.flags & kSyn) && (h.flags & kAck) && h.ack == iss_ + 1) {
      irs_ = h.seq;
      rcv_nxt_ = irs_ + 1;
      process_ack(h, false, now);
      ack_pending_ = true;  // third step of the handshake
    }
    return;
  }

  if (h.flags & kSyn) {
    // The peer did not get our answer and is repeating its SYN (or SYN-ACK). Answer again.
    if (h.seq == irs_) {
      ack_pending_ = true;
      if (role_ == Role::Server) resend_synack_ = true;
    }
    return;
  }

  const bool pure = dec->payload.empty() && !(h.flags & kFin);
  process_ack(h, pure, now);
  process_data(h, dec->payload);
}

void Connection::process_data(const Header& h, std::span<const std::uint8_t> payload) {
  if (h.flags & kProbe) ack_pending_ = true;
  const bool has_fin = (h.flags & kFin) != 0;
  if (payload.empty() && !has_fin) return;
  ack_pending_ = true;

  std::uint32_t seq = h.seq;
  if (has_fin) fin_seq_ = seq + static_cast<std::uint32_t>(payload.size());

  // Drop any front part we already have.
  if (seq_lt(seq, rcv_nxt_)) {
    const std::uint32_t cut = rcv_nxt_ - seq;
    if (cut >= payload.size()) {
      if (!payload.empty()) ++stats_.duplicate_segments;
      payload = {};
    } else {
      payload = payload.subspan(cut);
      seq = rcv_nxt_;
    }
  }

  if (!payload.empty()) {
    const std::uint32_t off = seq - rcv_nxt_;
    const std::size_t room = free_window();
    if (off >= room) {
      ++stats_.window_drops;
    } else {
      payload = payload.first(std::min<std::size_t>(payload.size(), room - off));
      if (off == 0) {
        recv_buf_.insert(recv_buf_.end(), payload.begin(), payload.end());
        rcv_nxt_ += static_cast<std::uint32_t>(payload.size());
        drain_out_of_order();
      } else {
        ++stats_.out_of_order;
        auto it = ooo_.find(seq);
        if (it == ooo_.end() || it->second.size() < payload.size()) {
          ooo_[seq] = std::vector<std::uint8_t>(payload.begin(), payload.end());
        }
      }
    }
  }

  if (fin_seq_ && !peer_fin_ && rcv_nxt_ == *fin_seq_) {
    ++rcv_nxt_;
    peer_fin_ = true;
  }
}

void Connection::drain_out_of_order() {
  while (!ooo_.empty()) {
    auto it = ooo_.begin();
    if (seq_gt(it->first, rcv_nxt_)) break;
    const std::uint32_t end = it->first + static_cast<std::uint32_t>(it->second.size());
    if (seq_gt(end, rcv_nxt_)) {
      const std::size_t cut = rcv_nxt_ - it->first;
      recv_buf_.insert(recv_buf_.end(), it->second.begin() + static_cast<std::ptrdiff_t>(cut), it->second.end());
      rcv_nxt_ = end;
    }
    ooo_.erase(it);
  }
}

// ---------------------------------------------------------------- acknowledgements and loss

void Connection::mark_lost(Segment& s) {
  if (s.state == SegState::Outstanding) {
    pipe_ -= s.len;
    s.state = SegState::Lost;
    ++lost_count_;
  }
}

void Connection::set_outstanding(Segment& s) {
  if (s.state == SegState::Lost) --lost_count_;
  if (s.state != SegState::Outstanding) pipe_ += s.len;
  s.state = SegState::Outstanding;
}

void Connection::mark_sacked(const std::vector<SackBlock>& blocks, std::size_t& newly) {
  for (const SackBlock& b : blocks) {
    if (seq_le(b.right, snd_una_) || seq_ge(b.left, b.right) || seq_gt(b.right, snd_nxt_)) continue;
    auto it = std::lower_bound(inflight_.begin(), inflight_.end(), b.left,
                               [](const Segment& s, std::uint32_t v) { return seq_lt(s.seq, v); });
    for (; it != inflight_.end() && seq_le(it->seq + it->len, b.right); ++it) {
      if (it->state == SegState::Sacked || (it->flags & (kSyn | kFin))) continue;
      if (it->state == SegState::Outstanding) pipe_ -= it->len;
      if (it->state == SegState::Lost) --lost_count_;
      it->state = SegState::Sacked;
      newly += it->len;
    }
  }
}

void Connection::enter_recovery() {
  in_recovery_ = true;
  recover_ = snd_nxt_;
  ++stats_.fast_recoveries;
  const std::size_t flight = snd_nxt_ - snd_una_;
  ssthresh_ = std::max<std::size_t>(flight / 2, 2 * cfg_.mss);
  if (cfg_.congestion_control) cwnd_ = ssthresh_;
}

void Connection::detect_loss(Time /*now*/) {
  bool any = false;
  if (cfg_.sack) {
    // A segment is presumed lost once at least three segments' worth of data above it has been selectively acknowledged.
    std::size_t sacked_above = 0;
    for (auto it = inflight_.rbegin(); it != inflight_.rend(); ++it) {
      if (it->state == SegState::Sacked) {
        sacked_above += it->len;
      } else if (it->state == SegState::Outstanding && !it->retx && !(it->flags & kSyn) &&
                 sacked_above >= 3 * cfg_.mss) {
        mark_lost(*it);
        any = true;
      }
    }
  } else if (dupacks_ == 3) {
    for (Segment& s : inflight_) {
      if (s.state == SegState::Outstanding) {
        mark_lost(s);
        any = true;
        break;
      }
    }
  }
  if (any && !in_recovery_) enter_recovery();
}

void Connection::cc_on_ack(std::size_t newly) {
  if (!cfg_.congestion_control || newly == 0) return;
  if (cwnd_ < ssthresh_) {
    cwnd_ += newly;  // slow start: one byte of window per byte acknowledged, so it doubles every round trip
    if (cwnd_ > ssthresh_) cwnd_ = std::max<std::size_t>(ssthresh_, cwnd_ - newly + 1);
  } else {
    cwnd_ += std::max<std::size_t>(1, cfg_.mss * newly / cwnd_);  // congestion avoidance: about one segment per round trip
  }
  cwnd_ = std::min<std::size_t>(cwnd_, 2 * cfg_.send_buffer);
}

void Connection::process_ack(const Header& h, bool pure, Time now) {
  if (!(h.flags & kAck)) return;
  const std::uint32_t ack = h.ack;
  if (seq_lt(ack, snd_una_) || seq_gt(ack, snd_nxt_)) return;
  rwnd_ = h.window;
  if (h.ts_ecr != 0) {
    // Every packet carries its own timestamp, so a retransmission can never be confused with the original
    // and the sample does not depend on which segment this ACK happens to cover.
    const std::uint32_t rtt = ts_now() - h.ts_ecr;
    if (rtt < 60'000'000U) rtt_.sample(rtt);
  }
  if (rwnd_ > 0) {
    persist_deadline_ = kNever;
    persist_interval_ = cfg_.min_rto;
  }

  std::size_t newly = 0;
  const bool advanced = seq_gt(ack, snd_una_);

  if (advanced) {
    while (!inflight_.empty() && seq_le(inflight_.front().seq + inflight_.front().len, ack)) {
      Segment& s = inflight_.front();
      if (s.state == SegState::Outstanding) pipe_ -= s.len;
      if (s.state == SegState::Lost) --lost_count_;
      if (s.state != SegState::Sacked && !(s.flags & (kSyn | kFin))) newly += s.len;
      if (!(s.flags & (kSyn | kFin))) stats_.bytes_acked += s.len;
      if (s.flags & kSyn) phase_ = Phase::Open;
      if (s.flags & kFin) fin_acked_ = true;
      inflight_.pop_front();
    }
    snd_una_ = ack;
    dupacks_ = 0;
    consecutive_timeouts_ = 0;
    // New data was acknowledged, so the path is alive: drop any exponential backoff.
    rto_ = rtt_.rto();
    if (in_recovery_) {
      if (seq_ge(ack, recover_)) {
        in_recovery_ = false;
        if (cfg_.congestion_control) cwnd_ = ssthresh_;
      } else if (!cfg_.sack) {
        // A partial acknowledgement exposes the next hole (NewReno).
        for (Segment& s : inflight_) {
          if (s.state == SegState::Outstanding) {
            mark_lost(s);
            break;
          }
        }
      }
    }
  } else if (pure && !inflight_.empty() && ack == snd_una_) {
    ++stats_.dup_acks;
    ++dupacks_;
  }

  std::size_t sacked = 0;
  if (cfg_.sack && !h.sack.empty()) mark_sacked(h.sack, sacked);

  if (!in_recovery_) cc_on_ack(newly + sacked);
  detect_loss(now);

  if (advanced) rto_deadline_ = inflight_.empty() ? kNever : now + rto_;
}

std::string Connection::check_invariants() const {
  std::uint64_t pipe = 0;
  std::size_t lost = 0;
  for (std::size_t i = 0; i < inflight_.size(); ++i) {
    const Segment& s = inflight_[i];
    if (s.state == SegState::Outstanding) pipe += s.len;
    if (s.state == SegState::Lost) ++lost;
    if (i + 1 < inflight_.size() && inflight_[i + 1].seq != s.seq + s.len) return "in-flight segments are not contiguous";
  }
  if (pipe != pipe_) return "pipe_ is " + std::to_string(pipe_) + " but outstanding segments add up to " + std::to_string(pipe);
  if (lost != lost_count_) return "lost_count_ is " + std::to_string(lost_count_) + " but " + std::to_string(lost) + " are lost";
  if (!inflight_.empty() && inflight_.back().seq + inflight_.back().len != snd_nxt_) return "snd_nxt_ does not follow the last segment";
  if (!inflight_.empty() && seq_gt(snd_una_, inflight_.front().seq)) return "snd_una_ is past the first in-flight segment";
  if (inflight_.empty() && snd_una_ != snd_nxt_ && phase_ != Phase::Idle) return "nothing in flight but snd_una_ != snd_nxt_";
  for (const auto& [seq, data] : ooo_) {
    if (seq_le(seq, rcv_nxt_)) return "out-of-order data at or behind rcv_nxt_";
    (void)data;
  }
  if (recv_buf_.size() > cfg_.recv_buffer) return "receive buffer over capacity";
  if (!inflight_.empty() && rto_deadline_ == kNever && phase_ != Phase::Failed) {
    bool any_outstanding = false;
    for (const Segment& s : inflight_) any_outstanding = any_outstanding || s.state == SegState::Outstanding;
    if (any_outstanding) return "segments outstanding but the retransmission timer is not running";
  }
  return {};
}

// ---------------------------------------------------------------- timers

Time Connection::next_timeout() const { return std::min(rto_deadline_, persist_deadline_); }

void Connection::on_timeout(Time now) {
  clock_ = now;
  if (phase_ == Phase::Failed) return;
  if (rto_deadline_ <= now && !inflight_.empty()) {
    ++stats_.timeouts;
    if (++consecutive_timeouts_ > cfg_.max_retries) {
      phase_ = Phase::Failed;
      rto_deadline_ = persist_deadline_ = kNever;
      return;
    }
    const std::size_t flight = snd_nxt_ - snd_una_;
    ssthresh_ = std::max<std::size_t>(flight / 2, 2 * cfg_.mss);
    if (cfg_.congestion_control) cwnd_ = cfg_.mss;  // back to a single segment and slow start
    in_recovery_ = false;
    dupacks_ = 0;
    for (Segment& s : inflight_) mark_lost(s);
    rto_ = std::min(rto_ * 2, cfg_.max_rto);  // exponential backoff
    rto_deadline_ = now + rto_;
  } else if (rto_deadline_ <= now) {
    rto_deadline_ = kNever;
  }
  if (persist_deadline_ <= now) {
    probe_pending_ = true;
    persist_interval_ = std::min(persist_interval_ * 2, cfg_.max_rto);
    persist_deadline_ = now + persist_interval_;
  }
}

// ---------------------------------------------------------------- outgoing

bool Connection::can_send(std::size_t len) const {
  if (!cfg_.congestion_control) return true;
  return pipe_ == 0 || pipe_ + len <= cwnd_;
}

std::vector<std::uint8_t> Connection::build(std::uint8_t flags, std::uint32_t seq, std::span<const std::uint8_t> payload) {
  Header h;
  h.flags = static_cast<std::uint8_t>(flags | kAck);
  h.seq = seq;
  h.ack = rcv_nxt_;
  h.ts_val = ts_now();
  h.ts_ecr = last_ts_rx_;
  h.window = static_cast<std::uint32_t>(free_window());
  last_advertised_ = free_window();
  if (cfg_.sack) {
    for (auto it = ooo_.begin(); it != ooo_.end() && h.sack.size() < kMaxSackBlocks;) {
      SackBlock b{it->first, it->first + static_cast<std::uint32_t>(it->second.size())};
      for (++it; it != ooo_.end() && seq_le(it->first, b.right); ++it) {
        const std::uint32_t end = it->first + static_cast<std::uint32_t>(it->second.size());
        if (seq_gt(end, b.right)) b.right = end;
      }
      h.sack.push_back(b);
    }
  }
  ++stats_.packets_sent;
  ack_pending_ = false;
  return encode(h, payload);
}

Connection::Segment* Connection::pick_segment(Time now) {
  // Segments the loss detector has given up on go first, oldest first.
  if (lost_count_ > 0) {
    for (Segment& s : inflight_) {
      if (s.state == SegState::Lost && ((s.flags & kSyn) || can_send(s.len))) return &s;
    }
  }
  if (phase_ != Phase::Open) return nullptr;

  const std::size_t queued = send_q_.size();
  if (queued == 0) {
    if (fin_queued_ && !fin_sent_ && can_send(1)) {
      Segment fin;
      fin.seq = snd_nxt_;
      fin.len = 1;
      fin.flags = kFin;
      ++snd_nxt_;
      fin_sent_ = true;
      inflight_.push_back(std::move(fin));
      ++lost_count_;
      return &inflight_.back();
    }
    return nullptr;
  }

  const std::size_t want = std::min(cfg_.mss, queued);
  const std::size_t in_use = snd_nxt_ - snd_una_;
  const std::size_t room = rwnd_ > in_use ? rwnd_ - in_use : 0;
  if (room < want) {
    // The receiver's window is too small to take a full segment. If nothing is in flight no ACK
    // will ever tell us it opened, so start the persist timer.
    if (inflight_.empty() && persist_deadline_ == kNever) persist_deadline_ = now + persist_interval_;
    return nullptr;
  }
  if (!can_send(want)) return nullptr;

  Segment s;
  s.seq = snd_nxt_;
  s.len = static_cast<std::uint32_t>(want);
  s.data.assign(send_q_.begin(), send_q_.begin() + static_cast<std::ptrdiff_t>(want));
  send_q_.erase(send_q_.begin(), send_q_.begin() + static_cast<std::ptrdiff_t>(want));
  snd_nxt_ += s.len;
  inflight_.push_back(std::move(s));
  ++lost_count_;
  return &inflight_.back();
}

std::optional<std::vector<std::uint8_t>> Connection::poll_transmit(Time now) {
  clock_ = now;
  if (phase_ == Phase::Idle || phase_ == Phase::Failed) return std::nullopt;

  if (resend_synack_) {
    resend_synack_ = false;
    return build(kSyn, iss_, {});
  }

  if (Segment* s = pick_segment(now)) {
    if (s->sent_once) {
      s->retx = true;
      ++stats_.retransmissions;
    }
    set_outstanding(*s);
    s->sent_once = true;
    s->sent = now;
    if (rto_deadline_ == kNever) rto_deadline_ = now + rto_;
    if (!s->data.empty()) ++stats_.data_segments_sent;
    if (s->flags == kSyn) {
      // A client SYN carries no ACK; a server SYN-ACK acknowledges the client's SYN.
      Header h;
      h.flags = static_cast<std::uint8_t>(kSyn | (role_ == Role::Server ? kAck : 0));
      h.seq = s->seq;
      h.ack = role_ == Role::Server ? rcv_nxt_ : 0;
      h.ts_val = ts_now();
      h.ts_ecr = last_ts_rx_;
      h.window = static_cast<std::uint32_t>(free_window());
      last_advertised_ = free_window();
      ++stats_.packets_sent;
      return encode(h, {});
    }
    return build(s->flags, s->seq, s->data);
  }

  if (probe_pending_) {
    probe_pending_ = false;
    return build(kProbe, snd_nxt_, {});
  }
  if (ack_pending_) return build(0, snd_nxt_, {});
  return std::nullopt;
}

}  // namespace rudp
