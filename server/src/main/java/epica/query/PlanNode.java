// PlanNode.java -- the physical plan: a tree of operators the Executor runs.
//
// The planner turns a Statement (what the user wants) into PlanNodes (how to
// get it). The interesting transformations:
//
//   SCAN PREFIX "user:"      -> RangeScan(start="user:", end="user;")
//       The exclusive upper bound is the prefix with its last byte
//       incremented, so a single engine range scan does the work and no
//       client-side prefix check is needed.
//
//   SCAN ... LIMIT 10        -> Limit(10, RangeScan(..., pushdownLimit=10))
//       Without a WHERE, the limit is pushed into the engine request, so the
//       server never sends more than 10 rows.
//
//   SCAN ... WHERE ... LIMIT 10 -> Limit(10, Filter(RangeScan(pageSize=128)))
//       With a WHERE the engine cannot apply the limit (it does not know the
//       predicate), so the scan streams pages of 128 rows until Limit is
//       satisfied -- bounded memory even for huge ranges.
//
//   COUNT ...                -> Count(Filter?(RangeScan))
//
// Each node's explain() renders the tree for EXPLAIN.
package epica.query;

import java.util.List;

public sealed interface PlanNode {
  String explain(int indent);

  static String pad(int n) { return "  ".repeat(n); }

  record PointGet(byte[] key) implements PlanNode {
    public String explain(int d) { return pad(d) + "PointGet(key=" + Bytes.show(key) + ")\n"; }
  }

  record PointPut(byte[] key, byte[] value) implements PlanNode {
    public String explain(int d) {
      return pad(d) + "PointPut(key=" + Bytes.show(key) + ", value=" + Bytes.show(value) + ")\n";
    }
  }

  record PointDelete(byte[] key) implements PlanNode {
    public String explain(int d) { return pad(d) + "PointDelete(key=" + Bytes.show(key) + ")\n"; }
  }

  /** Streams rows in [start, end) from the engine, pageSize at a time. pushdownLimit 0 = none. */
  record RangeScan(byte[] start, byte[] end, int pushdownLimit, int pageSize) implements PlanNode {
    public String explain(int d) {
      return pad(d) + "RangeScan(start=" + Bytes.show(start) + ", end="
          + (end.length == 0 ? "<unbounded>" : Bytes.show(end))
          + (pushdownLimit > 0 ? ", engineLimit=" + pushdownLimit : "")
          + ", pageSize=" + pageSize + ")\n";
    }
  }

  record Filter(byte[] valueContains, PlanNode child) implements PlanNode {
    public String explain(int d) {
      return pad(d) + "Filter(value CONTAINS " + Bytes.show(valueContains) + ")\n" + child.explain(d + 1);
    }
  }

  record Limit(int n, PlanNode child) implements PlanNode {
    public String explain(int d) { return pad(d) + "Limit(" + n + ")\n" + child.explain(d + 1); }
  }

  record Count(PlanNode child) implements PlanNode {
    public String explain(int d) { return pad(d) + "Count\n" + child.explain(d + 1); }
  }

  record AtomicBatch(List<epica.client.BatchOp> ops) implements PlanNode {
    public String explain(int d) {
      StringBuilder sb = new StringBuilder(pad(d) + "AtomicBatch(" + ops.size() + " ops)\n");
      for (epica.client.BatchOp op : ops) {
        sb.append(pad(d + 1)).append(op.isPut() ? "Put " : "Delete ").append(Bytes.show(op.key()));
        if (op.isPut()) sb.append(" = ").append(Bytes.show(op.value()));
        sb.append('\n');
      }
      return sb.toString();
    }
  }

  record Explain(PlanNode inner) implements PlanNode {
    public String explain(int d) { return pad(d) + "Explain\n" + inner.explain(d + 1); }
  }

  record Stats() implements PlanNode {
    public String explain(int d) { return pad(d) + "Stats\n"; }
  }
}
