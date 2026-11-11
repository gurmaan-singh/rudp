# Design notes

## One connection class with no I/O

`Connection` is a state machine. It never opens a socket, reads a clock or sleeps. The caller does four things:

```cpp
conn.on_datagram(bytes, now);                 // a packet arrived
while (auto d = conn.poll_transmit(now)) ...  // send these
conn.next_timeout();                          // wake me at this time
conn.on_timeout(now);                         // ...and call this when it comes
```

Two consequences. Tests run in virtual time: a 20 % loss transfer that would take a minute on a real network finishes in milliseconds and gives the same answer every run, so every failure is reproducible from a seed. And the same class runs on real sockets without changes (`rudp_send`, `rudp_recv`, and a loopback test).

## The simulator

`SimLink` is a one-way path with propagation delay, jitter, random loss, duplication, reordering (a packet is held back extra), bit corruption, and an optional bottleneck: a drop-tail queue in front of a link of fixed bandwidth. Delivery is a priority queue ordered by arrival time. `run_transfer` wires two connections and two links together and moves the clock to the next event: a delivery, a timer, or a slow reader's tick.

The bottleneck is what makes congestion control testable. Random loss alone does not tell a sender anything about the path; a full queue does.

## Wire format

32-byte header plus up to four SACK blocks. It carries sequence and acknowledgement numbers, the receive window in bytes, flags, a payload length, two timestamps and a CRC-32 over the whole packet. See `include/rudp/packet.hpp` for the layout.

Sequence numbers wrap at 2^32. All comparisons are signed differences (`seq_lt(a, b)` is `int32(a - b) < 0`), which is right as long as two numbers compared are less than 2^31 apart. A test starts the sequence space just below the wrap.

SYN and FIN each use one sequence number so they are retransmitted and acknowledged exactly like data. That removes a whole class of special cases in the handshake and close.

## Sender

Every sent segment stays in `inflight_` until cumulatively acknowledged. Each is in one of three states:

| State | Meaning |
|---|---|
| Outstanding | sent, no news |
| Sacked | the receiver reported it in a SACK block |
| Lost | the sender decided it is gone and must be sent again |

`pipe_`, the number of bytes believed to be in the network, is the total length of the Outstanding segments. It is kept up to date incrementally by two functions (`mark_lost`, `set_outstanding`) rather than recomputed, and `check_invariants()` verifies it. New segments start in the Lost state ("needs sending"), so sending a new segment and retransmitting an old one go through the same path.

Sending is allowed while `pipe_ + len <= cwnd`, and while the bytes between the oldest unacknowledged byte and the new one fit in the receiver's advertised window. Retransmissions go first.

**Loss detection.** With SACK, a segment is marked lost once at least three segments' worth of data above it has been selectively acknowledged. Without SACK, the third duplicate ACK marks the first outstanding segment. A segment that has been retransmitted is never re-marked by this rule, only by the timer; otherwise the old SACK information would mark it lost again the instant it was resent.

**Recovery.** The first loss in a window sets `ssthresh = max(flight/2, 2 MSS)` and `cwnd = ssthresh`, and remembers `snd_nxt`; recovery ends when the cumulative ACK passes it. Further losses in the same window are retransmitted without halving again.

**Timer.** One retransmission timer for the oldest unacknowledged segment, restarted when new data is acknowledged. When it fires: `ssthresh` is halved, `cwnd` becomes one segment, every segment that is not selectively acknowledged is marked lost, and the timeout doubles (up to 60 s). It is reset from the RTT estimate as soon as new data is acknowledged.

## Round-trip time

Every packet carries its sender's clock; every packet echoes the clock value of the most recent packet received. The sender subtracts the echo from now, and that is a sample, from any ACK, including duplicates. There is no ambiguity about retransmissions to work around (Karn's rule), and the sample does not depend on which segment the ACK happens to cover.

I started with the classic approach, sampling the send time of the newest newly-acknowledged segment. Under loss it gave multi-second samples on a 40 ms path: a segment sent early, then acknowledged only after a hole in front of it had been repaired by a retransmission timeout, looks like a very slow round trip. The estimate climbed to several seconds, the timeout with it, and a 300 KB transfer at 20 % loss did not finish in ten simulated minutes. After the change it finishes in 15 to 40 seconds.

## Receiver

In-order data goes to a byte buffer the application reads from; out-of-order data waits in a map keyed by sequence number. When the gap is filled, the map is drained. The advertised window is the free space in the buffer, and the right edge of the window never moves backward, which keeps anything accepted out of order inside the buffer when it is later drained.

SACK blocks are built from the map by merging adjacent entries, lowest first, at most four.

When the application reads and frees at least one segment of space since the last advertisement, the receiver sends an acknowledgement, so the sender does not wait on a stale zero window. If the sender has nothing in flight and the window is too small, the persist timer sends a probe packet (flag PROBE, no payload) with exponential backoff, which the receiver answers with its current window.

## Congestion control

Reno-style. Slow start adds one byte to the window per byte acknowledged until `ssthresh`; congestion avoidance adds about one segment per round trip. The window is capped at twice the send buffer.

One thing I got wrong first: I limited slow-start growth to two segments per ACK, as RFC 3465 suggests for delayed ACKs. In the simulator a burst arrives in one instant and produces one ACK, so growth fell to two segments per round trip and an unloaded 80 ms path took 4.5 s for 4 MB. Counting acknowledged bytes fixed it.

## Test strategy

The tests that matter are the ones that run whole transfers and compare every byte. They run on randomly generated networks with random segment sizes. Alongside, `check_invariants()` runs after every simulation step, so an accounting error is reported at the moment it happens rather than as a mysterious stall many seconds later.

## Not done

- Delayed ACKs, and a proper ACK-per-two-segments policy.
- RACK / tail loss probes, so a lost retransmission does not need a full timeout.
- Pacing; CUBIC or BBR.
- Multiple connections per socket, and a connection identifier.
- Path MTU discovery. The segment size is fixed by configuration.
