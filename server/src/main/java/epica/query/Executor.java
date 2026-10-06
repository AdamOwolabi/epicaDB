// Executor.java -- runs a PlanNode against an EngineClient.
//
// Row-producing nodes (RangeScan, Filter, Limit) are implemented as pull-based
// iterators ("Volcano model"): each operator's next() asks its child for a
// row, transforms/filters it, and hands it up. Nothing materialises the
// whole result set except the final collector, and Limit stops pulling as
// soon as it has enough -- so `SCAN PREFIX x WHERE ... LIMIT 1` over a
// million keys stops after the first match's page.
//
//   Limit(10)
//     Filter(value CONTAINS "ada")
//       RangeScan(pages of 128 from the engine)
//
// RangeScan pagination: request pageSize rows starting at `cursor`; after a
// full page, set cursor = lastKey + 0x00 (the smallest key strictly greater
// than lastKey) and ask again. A short page means the range is exhausted.
package epica.query;

import epica.client.EngineClient;
import epica.client.KeyValue;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Optional;

public final class Executor {
  private final EngineClient engine;

  public Executor(EngineClient engine) { this.engine = engine; }

  /** Convenience: parse + plan + execute one query line. */
  public QueryResult execute(String query) {
    Statement stmt = Parser.parse(query);
    PlanNode plan = new Planner().plan(stmt);
    return execute(plan);
  }

  public QueryResult execute(PlanNode plan) {
    if (plan instanceof PlanNode.PointGet g) {
      Optional<byte[]> v = engine.get(g.key());
      return v.map(QueryResult::value).orElseGet(QueryResult::notFound);
    }
    if (plan instanceof PlanNode.PointPut p) {
      engine.put(p.key(), p.value());
      return QueryResult.ok();
    }
    if (plan instanceof PlanNode.PointDelete d) {
      engine.delete(d.key());
      return QueryResult.ok();
    }
    if (plan instanceof PlanNode.AtomicBatch b) {
      engine.batch(b.ops());
      return QueryResult.ok();
    }
    if (plan instanceof PlanNode.Stats) return QueryResult.text(engine.stats());
    if (plan instanceof PlanNode.Explain e) return QueryResult.text(e.inner().explain(0));
    if (plan instanceof PlanNode.Count c) {
      long n = 0;
      for (Iterator<KeyValue> it = open(c.child()); it.hasNext(); it.next()) n++;
      return QueryResult.count(n);
    }
    // Row-producing root: collect.
    List<KeyValue> rows = new ArrayList<>();
    for (Iterator<KeyValue> it = open(plan); it.hasNext(); ) rows.add(it.next());
    return QueryResult.rows(rows);
  }

  // --- operator iterators ---

  private Iterator<KeyValue> open(PlanNode node) {
    if (node instanceof PlanNode.RangeScan s) return new ScanIterator(s);
    if (node instanceof PlanNode.Filter f) return new FilterIterator(f.valueContains(), open(f.child()));
    if (node instanceof PlanNode.Limit l) return new LimitIterator(l.n(), open(l.child()));
    throw new QueryException("executor: node does not produce rows: " + node.getClass().getSimpleName());
  }

  /** Pulls pages from the engine on demand. */
  private final class ScanIterator implements Iterator<KeyValue> {
    private final PlanNode.RangeScan scan;
    private byte[] cursor;
    private List<KeyValue> page = List.of();
    private int idx = 0;
    private boolean exhausted = false;
    private int fetched = 0;

    ScanIterator(PlanNode.RangeScan scan) {
      this.scan = scan;
      this.cursor = scan.start();
    }

    @Override
    public boolean hasNext() {
      if (idx < page.size()) return true;
      if (exhausted) return false;
      fetchPage();
      return idx < page.size();
    }

    @Override
    public KeyValue next() {
      if (!hasNext()) throw new NoSuchElementException();
      return page.get(idx++);
    }

    private void fetchPage() {
      int want = scan.pageSize();
      if (scan.pushdownLimit() > 0) {
        int remaining = scan.pushdownLimit() - fetched;
        if (remaining <= 0) {
          exhausted = true;
          page = List.of();
          idx = 0;
          return;
        }
        want = Math.min(want, remaining);
      }
      page = engine.scan(cursor, scan.end(), want);
      idx = 0;
      fetched += page.size();
      if (page.size() < want) {
        exhausted = true;  // short page: nothing more in range
      } else {
        cursor = Bytes.immediateSuccessor(page.get(page.size() - 1).key());
      }
    }
  }

  private static final class FilterIterator implements Iterator<KeyValue> {
    private final byte[] needle;
    private final Iterator<KeyValue> child;
    private KeyValue pending;

    FilterIterator(byte[] needle, Iterator<KeyValue> child) {
      this.needle = needle;
      this.child = child;
    }

    @Override
    public boolean hasNext() {
      while (pending == null && child.hasNext()) {
        KeyValue kv = child.next();
        if (Bytes.contains(kv.value(), needle)) pending = kv;
      }
      return pending != null;
    }

    @Override
    public KeyValue next() {
      if (!hasNext()) throw new NoSuchElementException();
      KeyValue kv = pending;
      pending = null;
      return kv;
    }
  }

  private static final class LimitIterator implements Iterator<KeyValue> {
    private final int n;
    private final Iterator<KeyValue> child;
    private int emitted = 0;

    LimitIterator(int n, Iterator<KeyValue> child) {
      this.n = n;
      this.child = child;
    }

    @Override
    public boolean hasNext() { return emitted < n && child.hasNext(); }

    @Override
    public KeyValue next() {
      if (!hasNext()) throw new NoSuchElementException();
      emitted++;
      return child.next();
    }
  }
}
