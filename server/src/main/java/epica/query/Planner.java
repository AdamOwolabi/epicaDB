// Planner.java -- Statement (AST) -> PlanNode (physical plan).
//
// This is where query semantics become engine calls. See PlanNode.java for
// the transformations and the reasoning behind each.
package epica.query;

import epica.client.BatchOp;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public final class Planner {
  /** Rows fetched per engine round trip when streaming a filtered scan. */
  public static final int PAGE_SIZE = 128;

  public PlanNode plan(Statement s) {
    if (s instanceof Statement.Get g) return new PlanNode.PointGet(Bytes.utf8(g.key()));
    if (s instanceof Statement.Put p) return new PlanNode.PointPut(Bytes.utf8(p.key()), Bytes.utf8(p.value()));
    if (s instanceof Statement.Delete d) return new PlanNode.PointDelete(Bytes.utf8(d.key()));
    if (s instanceof Statement.Stats) return new PlanNode.Stats();
    if (s instanceof Statement.Explain e) return new PlanNode.Explain(plan(e.inner()));
    if (s instanceof Statement.Batch b) {
      List<BatchOp> ops = new ArrayList<>();
      for (Statement.BatchOp op : b.ops()) {
        ops.add(op.isPut() ? BatchOp.put(Bytes.utf8(op.key()), Bytes.utf8(op.value()))
                           : BatchOp.delete(Bytes.utf8(op.key())));
      }
      return new PlanNode.AtomicBatch(ops);
    }
    if (s instanceof Statement.Scan sc) {
      boolean filtered = sc.where().isPresent();
      // Limit can only be pushed to the engine when no client-side filter
      // sits between the scan and the limit.
      int pushdown = (!filtered && sc.limit().isPresent()) ? sc.limit().get() : 0;
      PlanNode node = rangeScan(sc.range(), pushdown);
      node = maybeFilter(node, sc.where());
      if (sc.limit().isPresent()) node = new PlanNode.Limit(sc.limit().get(), node);
      return node;
    }
    if (s instanceof Statement.Count c) {
      PlanNode node = rangeScan(c.range(), 0);
      node = maybeFilter(node, c.where());
      return new PlanNode.Count(node);
    }
    throw new QueryException("planner: unsupported statement " + s);
  }

  private static PlanNode rangeScan(Statement.Range r, int pushdownLimit) {
    byte[] start, end;
    if (r.prefix().isPresent()) {
      start = Bytes.utf8(r.prefix().get());
      end = Bytes.prefixSuccessor(start);
    } else {
      start = r.from().map(Bytes::utf8).orElse(new byte[0]);
      end = r.to().map(Bytes::utf8).orElse(new byte[0]);
    }
    return new PlanNode.RangeScan(start, end, pushdownLimit, PAGE_SIZE);
  }

  private static PlanNode maybeFilter(PlanNode child, Optional<Statement.Predicate> where) {
    return where.map(p -> (PlanNode) new PlanNode.Filter(Bytes.utf8(p.valueContains()), child))
        .orElse(child);
  }
}
