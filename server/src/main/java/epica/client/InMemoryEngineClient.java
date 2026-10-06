// InMemoryEngineClient.java -- a TreeMap pretending to be the engine.
//
// Same contract as the real thing (sorted keys, [start,end) scans, atomic
// batches, NOT_FOUND as Optional.empty) so the query layer's unit tests and
// the `memory` shell mode work without a running C++ server. Byte order is
// unsigned lexicographic, matching Slice::compare in the engine.
package epica.client;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;

public final class InMemoryEngineClient implements EngineClient {
  /** Unsigned lexicographic byte comparison, the engine's key order. */
  public static final Comparator<byte[]> BYTE_ORDER = Arrays::compareUnsigned;

  private final TreeMap<byte[], byte[]> map = new TreeMap<>(BYTE_ORDER);
  private long ops = 0;

  @Override
  public synchronized Optional<byte[]> get(byte[] key) {
    ops++;
    return Optional.ofNullable(map.get(key));
  }

  @Override
  public synchronized void put(byte[] key, byte[] value) {
    ops++;
    map.put(key.clone(), value.clone());
  }

  @Override
  public synchronized void delete(byte[] key) {
    ops++;
    map.remove(key);
  }

  @Override
  public synchronized List<KeyValue> scan(byte[] start, byte[] end, int limit) {
    ops++;
    List<KeyValue> out = new ArrayList<>();
    for (Map.Entry<byte[], byte[]> e : map.tailMap(start, true).entrySet()) {
      if (end.length > 0 && BYTE_ORDER.compare(e.getKey(), end) >= 0) break;
      out.add(new KeyValue(e.getKey(), e.getValue()));
      if (limit > 0 && out.size() >= limit) break;
    }
    return out;
  }

  @Override
  public synchronized void batch(List<BatchOp> batch) {
    ops++;
    for (BatchOp op : batch) {
      if (op.isPut()) map.put(op.key().clone(), op.value().clone());
      else map.remove(op.key());
    }
  }

  @Override
  public synchronized String stats() {
    return "in-memory engine: " + map.size() + " keys, " + ops + " operations\n";
  }

  public synchronized int size() { return map.size(); }
}
