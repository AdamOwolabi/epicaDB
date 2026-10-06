// EngineClient.java -- what the query layer needs from a storage engine.
//
// There are two implementations:
//   SocketEngineClient   talks to the real C++ engine over TCP
//   InMemoryEngineClient a TreeMap stand-in so the parser/planner/executor
//                        can be unit-tested (and demoed) without a server.
//
// Keeping the executor programmed against this interface is what makes the
// query layer testable in milliseconds; it is also the seam where a future
// sharded/replicated engine would plug in.
package epica.client;

import java.util.List;
import java.util.Optional;

public interface EngineClient extends AutoCloseable {
  Optional<byte[]> get(byte[] key);

  void put(byte[] key, byte[] value);

  void delete(byte[] key);

  /**
   * Keys in [start, end) in sorted order. end == empty means unbounded.
   * limit == 0 means no limit.
   */
  List<KeyValue> scan(byte[] start, byte[] end, int limit);

  /** Applies all ops atomically: all persist or none do. */
  void batch(List<BatchOp> ops);

  String stats();

  @Override
  default void close() {}
}
