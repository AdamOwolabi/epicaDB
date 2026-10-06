// ExecutorTest.java -- end-to-end query execution against the in-memory
// engine, including pagination behaviour of the streaming scan.
package epica.query;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import epica.client.BatchOp;
import epica.client.EngineClient;
import epica.client.InMemoryEngineClient;
import epica.client.KeyValue;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

class ExecutorTest {
  private InMemoryEngineClient engine;
  private Executor ex;

  @BeforeEach
  void setUp() {
    engine = new InMemoryEngineClient();
    ex = new Executor(engine);
  }

  @Test
  void putGetDelete() {
    assertEquals(QueryResult.Kind.OK, ex.execute("PUT a \"hello world\"").kind());
    QueryResult r = ex.execute("GET a");
    assertEquals(QueryResult.Kind.VALUE, r.kind());
    assertEquals("hello world", Bytes.str(r.value()));
    assertEquals(QueryResult.Kind.OK, ex.execute("DEL a").kind());
    assertEquals(QueryResult.Kind.NOT_FOUND, ex.execute("GET a").kind());
  }

  @Test
  void prefixScanFilterLimitCount() {
    for (int i = 0; i < 20; i++) ex.execute("PUT user:" + String.format("%02d", i) + " name" + (i % 3));
    ex.execute("PUT other:1 name0");

    QueryResult all = ex.execute("SCAN PREFIX user:");
    assertEquals(20, all.rows().size());
    assertEquals("user:00", all.rows().get(0).keyString());

    QueryResult filtered = ex.execute("SCAN PREFIX user: WHERE VALUE CONTAINS name0 LIMIT 3");
    assertEquals(3, filtered.rows().size());
    for (KeyValue kv : filtered.rows()) assertEquals("name0", kv.valueString());

    assertEquals(7, ex.execute("COUNT PREFIX user: WHERE VALUE CONTAINS name0").count());
    assertEquals(21, ex.execute("COUNT").count());
    assertEquals(5, ex.execute("SCAN FROM user:10 TO user:15").rows().size());
  }

  @Test
  void batchIsAppliedInOrder() {
    ex.execute("BATCH PUT a 1; PUT b 2; DEL a END");
    assertEquals(QueryResult.Kind.NOT_FOUND, ex.execute("GET a").kind());
    assertEquals("2", Bytes.str(ex.execute("GET b").value()));
  }

  @Test
  void explainAndStatsReturnText() {
    assertTrue(ex.execute("EXPLAIN GET k").text().contains("PointGet"));
    assertTrue(ex.execute("STATS").text().contains("in-memory"));
  }

  @Test
  void scanPaginatesAndStopsEarlyOnLimit() {
    // Wrap the engine to count scan round trips.
    AtomicInteger scans = new AtomicInteger();
    EngineClient counting = new EngineClient() {
      public Optional<byte[]> get(byte[] k) { return engine.get(k); }
      public void put(byte[] k, byte[] v) { engine.put(k, v); }
      public void delete(byte[] k) { engine.delete(k); }
      public List<KeyValue> scan(byte[] s, byte[] e, int limit) {
        scans.incrementAndGet();
        return engine.scan(s, e, limit);
      }
      public void batch(List<BatchOp> ops) { engine.batch(ops); }
      public String stats() { return engine.stats(); }
    };
    Executor cx = new Executor(counting);
    for (int i = 0; i < 1000; i++) cx.execute("PUT k" + String.format("%04d", i) + " v" + (i % 2));

    scans.set(0);
    // Unfiltered: limit pushed down -> exactly one round trip of 5 rows.
    assertEquals(5, cx.execute("SCAN LIMIT 5").rows().size());
    assertEquals(1, scans.get());

    scans.set(0);
    // Filtered + limit: pages of 128; first page already holds 5 matches.
    assertEquals(5, cx.execute("SCAN WHERE VALUE CONTAINS v1 LIMIT 5").rows().size());
    assertEquals(1, scans.get());

    scans.set(0);
    // Full count: 1000 rows / 128 per page = 8 pages (last one short).
    assertEquals(1000, cx.execute("COUNT").count());
    assertEquals(8, scans.get());
  }
}
